// Regnum — инспектор персонажа: шапка (портрет, имя, фракция, титул), вкладка «Сведения» (портрет из PNG/JPEG,
// имя, титул, фракция, отметка героя, содержание — расход на специалистов ТЗ 1.e.i, заметки, карточка кампании)
// и вкладка «Роли» (правитель, лорд провинций, места в совете, герой и полководец войск — ТЗ 1.a.vi, 1.b.iv, 1.c.iii)
// с назначением и снятием.
#include <algorithm>

#include "app/app_internal.h"
#include "app/widgets.h"

namespace rg::app::chars {   // объявления из character_common.cpp (struct Roles — точная копия)
struct Roles {
  std::vector<Id> rulerOf;
  std::vector<Id> lordOf;
  std::vector<std::pair<Id, Id>> seats;
  std::vector<Id> armies;
  std::vector<Id> commands;
  size_t total() const { return rulerOf.size() + lordOf.size() + seats.size() + armies.size(); }
};
Roles rolesOf(const World& w, Id character);
std::string rolesText(const World& w, const Roles& r);
const gfx::Image* portraitImage(const Character& c, bool square);
void avatar(const Character& c, float size, bool ring, std::string_view tip);
void askDelete(App& a, Id character);
void loadPortrait(App& a, Id character);
void askClearPortrait(App& a, Id character);
std::optional<std::string> findCanonCard(App& a, std::string_view entity, std::string_view name, std::string* foundId);
std::string positionOf(const World& w, Id faction, Id seat);
}  // namespace rg::app::chars

namespace rg::app::edkit {   // editors/modifiers.cpp — фишки с переносом
void chipsBegin();
ui::ChipAction chip(std::string_view label, const ui::ChipOpt& o);
void chipsEnd();
}  // namespace rg::app::edkit

namespace rg::app {
namespace {

std::string orName(const std::string& s, const char* fallback) { return s.empty() ? std::string(fallback) : s; }
std::string armyName(const World& w, Id army) { return detail::entityName(w, {SelType::Army, army}); }

// ---------------------------------------------------------------- шапка
void drawHeader(App& a, Id cid) {
  const World& w = a.world();
  const Character* c = w.character(cid);
  if (!c) return;
  chars::Roles r = chars::rolesOf(w, cid);
  bool ruler = !r.rulerOf.empty();
  ui::Row row({ui::px(64), ui::fr(1)}, ui::kAuto, 12);
  {
    ui::Group g(64, 0);
    chars::avatar(*c, 56, ruler, ruler ? "Правитель" : std::string_view());
  }
  {
    ui::Group g(0, 4);
    ui::caption(c->hero ? "Персонаж · значимый герой" : "Персонаж");
    ui::label(orName(c->name, "Без имени"), {.font = ui::Font::Heading});
    // Фракция и титул — фишками с переносом (длинные названия не обрезаются).
    edkit::chipsBegin();
    if (const Faction* f = w.faction(c->faction)) {
      ui::ChipOpt co;
      co.color = f->color;
      co.clickable = true;
      co.tooltip = f->isGuild() ? "Торговая гильдия — открыть" : "Государство — открыть";
      if (edkit::chip(orName(f->name, "Без названия"), co) == ui::ChipAction::Click) a.select(SelType::Faction, f->id);
    } else {
      edkit::chip("Без фракции", {.icon = "unlink"});
    }
    if (!c->title.empty()) edkit::chip(c->title, {.icon = ruler ? "crown" : "character", .tone = ruler ? ui::Tone::Accent : ui::Tone::Neutral});
    edkit::chipsEnd();
  }
}

// ---------------------------------------------------------------- вкладка «Сведения»
void portraitBlock(App& a, const Character& c, const chars::Roles& roles, bool ro) {
  const ui::Theme& th = ui::theme();
  Id cid = c.id;
  ui::Row row({ui::px(132), ui::fr(1)}, ui::kAuto, 14);
  {
    ui::Group g(132, 0);
    RectF pr = ui::next(132, 176);
    const gfx::Image* img = chars::portraitImage(c, false);
    if (img && !img->empty()) {
      ui::draw::shadow(pr, 10, 14, th.shadow.alpha(0.6f), 4);
      ui::draw::image(*img, pr, 10);
      ui::draw::rectStroke(pr, th.borderStrong.alpha(0.6f), 10, 1);
    } else {
      ui::draw::rect(pr, th.surface3, 10);
      ui::draw::rectStroke(pr, th.border, 10, 1);
      RectF ic{pr.cx() - 22, pr.cy() - 34, 44, 44};
      ui::draw::icon("portrait", ic, th.textMuted);
      ui::draw::text(c.portrait.empty() ? "Нет портрета" : "Загрузка…", RectF{pr.x, ic.bottom() + 8, pr.w, 18}, ui::Font::Small, th.textMuted,
                     ui::Align::Center);
    }
    a.markUi("character.portrait", pr);
  }
  {
    ui::Group g(0, 8);
    {
      ui::Disabled dis(ro);
      if (ui::button(c.portrait.empty() ? "Загрузить портрет" : "Заменить портрет", {.icon = "upload", .fill = true})) chars::loadPortrait(a, cid);
      ui::tooltip("Изображение PNG или JPEG; большие уменьшаются до 512 точек");
      a.markUi("character.loadPortrait");
      if (!c.portrait.empty()) {
        if (ui::button("Убрать портрет", {.variant = ui::Variant::Subtle, .icon = "trash", .fill = true})) chars::askClearPortrait(a, cid);
        a.markUi("character.clearPortrait");
      }
    }
    ui::stat(fmtNum(c.upkeep, std::fabs(c.upkeep - std::round(c.upkeep)) > 1e-9 ? 1 : 0), "Содержание за ход",
             {.icon = "coins", .tone = ui::Tone::Warning, .tooltip = "Платит фракция, которой персонаж служит: правитель, советник или герой — расход «специалисты» (ТЗ 1.e.i)"});
    ui::stat(fmtInt(i64(roles.total())), plural(i64(roles.total()), "Роль", "Роли", "Ролей"),
             {.icon = "council", .tone = ui::Tone::Info, .tooltip = roles.total() ? chars::rolesText(a.world(), roles) : std::string_view("Пока без ролей")});
  }
}

void drawInfo(App& a, Id cid) {
  const World& w = a.world();
  const Character* cp = w.character(cid);
  if (!cp) return;
  const Character& c = *cp;
  bool ro = a.readOnly();
  chars::Roles roles = chars::rolesOf(w, cid);
  portraitBlock(a, c, roles, ro);
  if (ui::Section s("Основное", "user"); s) {
    ui::prop("Имя", "edit");
    std::string name = c.name;
    if (ui::textField("name", name, {.placeholder = "Имя персонажа", .maxLength = 80, .readOnly = ro}) && trim(name) != c.name)
      a.act("Имя персонажа", [&](Tx& tx) { tx.character(cid).name = trim(name); });
    a.markUi("character.name");
    ui::prop("Титул", "crown");
    std::string title = c.title;
    if (ui::textField("title", title, {.placeholder = "Король, леди, магистр…", .maxLength = 80, .readOnly = ro}) && trim(title) != c.title)
      a.act("Титул персонажа", [&](Tx& tx) { tx.character(cid).title = trim(title); });
    a.markUi("character.title");
    ui::prop("Фракция", "flag");
    Id fac = c.faction;
    if (w::factionPicker("faction", fac, w::FactionFilter::Any, "Без фракции", 0, ro)) {
      int left = 0;
      a.act("Фракция персонажа", [&](Tx& tx) {
        // Герой сопровождает только отряды своей фракции: из чужих войск он уходит (rules::setHero).
        for (Id army : roles.armies) {
          const Army* ar = tx.w().army(army);
          if (!ar) continue;
          bool inOther = false;
          for (const ArmyGroup& g : ar->groups)
            if (g.faction != fac && std::find(g.heroes.begin(), g.heroes.end(), cid) != g.heroes.end()) inOther = true;
          if (inOther) {
            rules::setHero(tx, army, cid, false);
            left++;
          }
        }
        tx.character(cid).faction = fac;
      });
      if (left) a.toast("«" + orName(c.name, "Персонаж") + "» покинул войска прежней фракции: " + std::to_string(left), ToastKind::Info, "army");
    }
    a.markUi("character.faction");
    bool hero = c.hero;
    if (ui::toggle("Значимый герой фракции", hero, ro)) a.act(hero ? "Отметить героем" : "Снять отметку героя", [&](Tx& tx) { tx.character(cid).hero = hero; });
    a.markUi("character.hero");
    ui::prop("Содержание", "coins");
    double up = c.upkeep;
    if (ui::numberField("upkeep", up, {.min = 0, .max = 1e12, .step = 1, .digits = 1, .unit = "за ход", .disabled = ro,
                                       .tooltip = "Начисляется, пока персонаж правитель, советник или герой фракции (ТЗ 1.e.i)"}))
      a.act("Содержание персонажа", [&](Tx& tx) { tx.character(cid).upkeep = std::max(0.0, up); }, {.coalesce = "upkeep:" + std::to_string(cid)});
    a.markUi("character.upkeep");
    if (c.faction) {
      // Содержание входит в расход «специалисты», только пока персонаж служит фракции (правило RULES.md §3).
      const Faction* f = w.faction(c.faction);
      bool serves = f && (f->ruler == cid || (c.hero && c.faction == f->id));
      if (f && !serves)
        for (const auto& seat : f->council) if (seat.character == cid) serves = true;
      auto calc = rules::calc(w);
      const rules::FactionCalc* fc = calc->faction(c.faction);
      if (fc && serves) {
        double share = fc->expSpecialists > 0 ? c.upkeep / fc->expSpecialists : 0;
        ui::label("Специалисты «" + w.factionName(c.faction) + "»: " + fmtNum(fc->expSpecialists) + " за ход", {.font = ui::Font::Small, .ink = ui::Ink::Muted});
        ui::progress(share, {.tone = ui::Tone::Warning, .height = 4, .text = fmtPct(share * 100)});
      } else if (fc) {
        ui::label("Без должности и не герой — содержание не начисляется", {.font = ui::Font::Small, .ink = ui::Ink::Muted, .wrap = true});
      }
    }
  }
  if (ui::Section s("Заметки", "note", {.defaultOpen = !c.notes.empty()}); s) {
    std::string notes = c.notes;
    if (ui::textArea("notes", notes, 96, {.placeholder = "Характер, история, цели…", .readOnly = ro}) && notes != c.notes)
      a.act("Заметки о персонаже", [&](Tx& tx) { tx.character(cid).notes = notes; });
    a.markUi("character.notes");
  }
  if (ui::Section s("Канон", "book", {.defaultOpen = true}); s) {
    ui::Row r({ui::fr(1), ui::px(30), ui::px(30)}, 30, 6);
    std::string ent = c.entity;
    if (ui::textField("entity", ent, {.placeholder = "ID карточки, напр. CHAR-0001", .icon = "link", .maxLength = 40, .readOnly = ro,
                                      .tooltip = "Связь с карточкой персонажа в тексте кампании"}) &&
        trim(ent) != c.entity)
      a.act("Карточка персонажа", [&](Tx& tx) { tx.character(cid).entity = trim(ent); });
    a.markUi("character.entity");
    if (ui::iconButton("external", "Открыть карточку кампании", {.disabled = c.entity.empty()})) {
      if (auto p = chars::findCanonCard(a, c.entity, {}, nullptr)) platform::openPath(*p);
      else a.toast("Карточка " + c.entity + " не найдена рядом с проектом", ToastKind::Warning, "book");
    }
    a.markUi("character.openCard");
    if (ui::iconButton("search", "Найти карточку по имени", {.disabled = ro || c.name.empty()})) {
      std::string id;
      if (auto p = chars::findCanonCard(a, {}, c.name, &id); p && !id.empty()) {
        a.act("Карточка персонажа", [&](Tx& tx) { tx.character(cid).entity = id; });
        a.toast("Найдена карточка " + id, ToastKind::Success, "book");
      } else {
        a.toast("Карточка «" + c.name + "» не найдена", ToastKind::Info, "book");
      }
    }
    a.markUi("character.findCard");
  }
  ui::spacer(4);
  {
    ui::Disabled dis(ro);
    if (ui::button("Удалить персонажа", {.variant = ui::Variant::Danger, .icon = "trash", .fill = true})) chars::askDelete(a, cid);
    a.markUi("character.delete");
  }
}

// ---------------------------------------------------------------- вкладка «Роли»
void removeButton(const char* tip, bool ro, const std::function<void()>& fn) {
  if (ui::iconButton("close", tip, {.size = ui::Size::Small, .disabled = ro})) fn();
}

void drawRoles(App& a, Id cid) {
  const World& w = a.world();
  const Character* cp = w.character(cid);
  if (!cp) return;
  const Character& c = *cp;
  bool ro = a.readOnly();
  chars::Roles r = chars::rolesOf(w, cid);
  const Faction* own = w.faction(c.faction);
  bool state = own && own->isState();
  std::string who = "«" + orName(c.name, "Персонаж") + "»";

  // Правитель (ТЗ 1.b.iv).
  {
    std::string badge = std::to_string(r.rulerOf.size());
    ui::Section s("Правитель", "crown", {.badge = r.rulerOf.empty() ? std::string_view() : std::string_view(badge)});
    a.markUi("character.roles.ruler");
    if (s) {
      for (Id fid : r.rulerOf) {
        const Faction* f = w.faction(fid);
        if (!f) continue;
        ui::IdScope sc{i64(fid)};
        ui::Row row({ui::fr(1), ui::px(24)}, 28, 6);
        {
          ui::HStack hs(28, ui::Align::Left, 6);
          w::factionChip(fid, true);
          if (!f->rulerTitle.empty()) ui::label(f->rulerTitle, {.ink = ui::Ink::Dim});
        }
        removeButton("Снять с правления", ro, [&] { a.act("Снять правителя", [&](Tx& tx) { tx.faction(fid).ruler = 0; }); });
      }
      if (state && own->ruler != cid) {
        std::string label = "Сделать правителем «" + orName(own->name, "государства") + "»";
        ui::Disabled dis(ro);
        if (ui::button(label, {.icon = "crown", .fill = true})) {
          Id fid = own->id;
          std::string title = c.title;
          auto apply = [fid, cid, title](App& x) {
            x.act("Новый правитель", [&](Tx& tx) {
              Faction& f = tx.faction(fid);
              f.ruler = cid;
              if (!title.empty()) f.rulerTitle = title;
            });
          };
          if (own->ruler && w.character(own->ruler))
            a.confirm("Сменить правителя?", "Сейчас правит «" + w.characterName(own->ruler) + "». Трон перейдёт к " + who + ".", "Сменить", false, apply);
          else
            apply(a);
        }
        a.markUi("character.makeRuler");
      } else if (r.rulerOf.empty()) {
        ui::label(own ? "Правители бывают только у государств." : "Назначьте фракцию, чтобы сделать правителем.", {.font = ui::Font::Small, .ink = ui::Ink::Muted});
      }
    }
  }
  // Лорд провинций (ТЗ 1.a.vi).
  {
    std::string badge = std::to_string(r.lordOf.size());
    ui::Section s("Лорд провинций", "lord", {.badge = r.lordOf.empty() ? std::string_view() : std::string_view(badge)});
    a.markUi("character.roles.lord");
    if (s) {
      if (!r.lordOf.empty()) {
        edkit::chipsBegin();
        for (Id pid : r.lordOf) {
          const Province* p = w.province(pid);
          ui::IdScope sc{i64(pid)};
          ui::ChipOpt co;
          co.icon = "province";
          co.color = w::factionColor(w, p ? p->owner : 0);
          co.clickable = true;
          co.removable = !ro;
          co.tooltip = "Открыть провинцию";
          auto act = edkit::chip(orName(p ? p->name : std::string(), "Без названия"), co);
          if (act == ui::ChipAction::Click) a.select(SelType::Province, pid, true);
          if (act == ui::ChipAction::Remove) a.act("Снять лорда провинции", [&](Tx& tx) { tx.province(pid).lord = 0; });
        }
        edkit::chipsEnd();
      }
      if (!ro) {
        std::vector<const Province*> ps;
        w.provinces.each([&](const Province& p) {
          if (!p.sea && p.lord != cid) ps.push_back(&p);
        });
        Id f = c.faction;
        std::sort(ps.begin(), ps.end(), [&](const Province* x, const Province* y) {
          bool ox = f && x->owner == f, oy = f && y->owner == f;
          if (ox != oy) return ox;
          return compareRu(x->name, y->name) < 0;
        });
        std::vector<std::string> hints(ps.size());
        for (size_t i = 0; i < ps.size(); i++) hints[i] = ps[i]->lord ? "лорд: " + w.characterName(ps[i]->lord) : std::string("без лорда");
        int idx = -1;
        if (ui::combo("lordof", idx, int(ps.size()),
                      [&](int i) { return ui::Option{ps[size_t(i)]->name, "province", w::factionColor(w, ps[size_t(i)]->owner), hints[size_t(i)]}; },
                      {.placeholder = "Назначить лордом провинции", .search = 1, .icon = "plus", .popupWidth = 320}) &&
            idx >= 0 && idx < int(ps.size())) {
          Id pid = ps[size_t(idx)]->id;
          a.act("Назначить лорда провинции", [&](Tx& tx) { tx.province(pid).lord = cid; });
        }
        a.markUi("character.addLord");
      }
    }
  }
  // Совет государства (ТЗ 1.b.iv: «Назначение в совете, назначенный лорд»).
  {
    std::string badge = std::to_string(r.seats.size());
    ui::Section s("Совет", "council", {.badge = r.seats.empty() ? std::string_view() : std::string_view(badge)});
    a.markUi("character.roles.council");
    if (s) {
      for (auto [fid, sid] : r.seats) {
        ui::IdScope sc{i64(sid)};
        ui::Row row({ui::fr(1), ui::fr(1), ui::px(24)}, 28, 6);
        ui::label(chars::positionOf(w, fid, sid), {.icon = "council"});
        w::factionChip(fid);
        removeButton("Освободить место в совете", ro, [&, fid = fid, sid = sid] {
          a.act("Освободить место в совете", [&](Tx& tx) {
            for (CouncilSeat& st : tx.faction(fid).council)
              if (st.id == sid) st.character = 0;
          });
        });
      }
      if (state && !ro) {
        // Пункты: свободные места государства, затем должности справочника (новое место).
        struct Opt {
          Id seat = 0;
          std::string position, label, hint;
        };
        std::vector<Opt> opts;
        for (const CouncilSeat& st : own->council)
          if (!st.character) opts.push_back({st.id, st.position, orName(st.position, "Советник"), "свободно"});
        for (const CatalogItem& pos : w.catalogs->positions) {
          bool exists = false;
          for (const CouncilSeat& st : own->council) exists = exists || utf8::searchKey(st.position) == utf8::searchKey(pos.name);
          if (!exists) opts.push_back({0, pos.name, pos.name, "новое место"});
        }
        int idx = -1;
        if (ui::combo("seat", idx, int(opts.size()), [&](int i) { return ui::Option{opts[size_t(i)].label, "council", {}, opts[size_t(i)].hint}; },
                      {.placeholder = "Занять место в совете", .icon = "plus", .disabled = opts.empty(),
                       .popupWidth = 300}) &&
            idx >= 0 && idx < int(opts.size())) {
          Opt o = opts[size_t(idx)];
          Id fid = own->id;
          a.act("Место в совете", [&](Tx& tx) {
            Faction& f = tx.faction(fid);
            if (o.seat) {
              for (CouncilSeat& st : f.council)
                if (st.id == o.seat) st.character = cid;
            } else {
              CouncilSeat st;
              st.id = tx.nextId(Seq::Council);
              st.position = o.position;
              st.character = cid;
              tx.faction(fid).council.push_back(st);
            }
          });
        }
        a.markUi("character.addSeat");
      } else if (r.seats.empty()) {
        ui::label(own ? "Совет есть только у государств." : "Назначьте государство, чтобы занять место в совете.", {.font = ui::Font::Small, .ink = ui::Ink::Muted});
      }
    }
  }
  // Войска и флот: герой и главный полководец (ТЗ 1.c.iii).
  {
    std::string badge = std::to_string(r.armies.size());
    ui::Section s("Войска и флот", "army", {.badge = r.armies.empty() ? std::string_view() : std::string_view(badge)});
    a.markUi("character.roles.armies");
    if (s) {
      for (Id aid : r.armies) {
        const Army* ar = w.army(aid);
        if (!ar) continue;
        ui::IdScope sc{i64(aid)};
        ui::Row row({ui::fr(1), ui::px(120), ui::px(24)}, 30, 6);
        ui::ChipOpt co;
        co.icon = ar->isFleet() ? "fleet" : "army";
        co.color = w::factionColor(w, ar->leader());
        co.clickable = true;
        co.tooltip = "Открыть и показать на карте";
        if (ui::chip(armyName(w, aid), co) == ui::ChipAction::Click) a.select(SelType::Army, aid, true);
        bool cmd = ar->commander == cid;
        if (ui::checkbox(ar->isFleet() ? "Флотоводец" : "Полководец", cmd, ro)) {
          a.act(cmd ? "Главный полководец" : "Снять главного полководца", [&](Tx& tx) { rules::setCommander(tx, aid, cmd ? cid : 0); });
        }
        removeButton(ar->isFleet() ? "Покинуть флот" : "Покинуть войско", ro,
                     [&] { a.act("Герой покинул войско", [&](Tx& tx) { rules::setHero(tx, aid, cid, false); }); });
      }
      if (c.faction && !ro) {
        std::vector<const Army*> list;
        w.armies.each([&](const Army& ar) {
          if (std::find(r.armies.begin(), r.armies.end(), ar.id) != r.armies.end()) return;
          for (const ArmyGroup& g : ar.groups)
            if (g.faction == c.faction) {
              list.push_back(&ar);
              return;
            }
        });
        std::vector<std::string> names(list.size());
        for (size_t i = 0; i < list.size(); i++) names[i] = armyName(w, list[i]->id);
        int idx = -1;
        if (ui::combo("army", idx, int(list.size()),
                      [&](int i) {
                        const Army* ar = list[size_t(i)];
                        return ui::Option{names[size_t(i)], ar->isFleet() ? "fleet" : "army", w::factionColor(w, ar->leader()),
                                          ar->commander ? std::string_view("есть полководец") : std::string_view()};
                      },
                      {.placeholder = list.empty() ? "Нет войск фракции на карте" : "Сопровождать войско или флот", .icon = "plus",
                       .disabled = list.empty(), .popupWidth = 320}) &&
            idx >= 0 && idx < int(list.size())) {
          Id aid = list[size_t(idx)]->id;
          a.act("Герой в войске", [&](Tx& tx) { rules::setHero(tx, aid, cid, true); });
        }
        a.markUi("character.addArmy");
      } else if (r.armies.empty()) {
        ui::label("Героями войск бывают персонажи фракции.", {.font = ui::Font::Small, .ink = ui::Ink::Muted});
      }
    }
  }
}

int rolesBadge(App& a, Id cid) { return int(chars::rolesOf(a.world(), cid).total()); }

HeaderReg header({"character.header", SelType::Character, 0, drawHeader});
TabReg tabInfo({"character.info", "user", "Сведения", 10, SelType::Character, nullptr, drawInfo});
TabReg tabRoles({"character.roles", "council", "Роли", 20, SelType::Character, nullptr, drawRoles, rolesBadge});

}  // namespace
}  // namespace rg::app
