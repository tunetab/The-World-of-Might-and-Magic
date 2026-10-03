// Regnum — вкладки земель: «Провинции» государства (ТЗ 1.b.iv: таблица владений — название, правитель провинции,
// ресурс, население, торговая ценность; расы государства — автоматически по провинциям) и «Штабы» гильдии
// (ТЗ 1.d.ii, 1.d.iv: провинции штабов, влияние и доход, открытие и закрытие штабов).
#include "app/panels/faction_common.h"

namespace rg::app {
namespace {

using namespace fac;

std::string provName(const Province& p) { return p.name.empty() ? std::string("Без названия") : p.name; }

// ---------------------------------------------------------------- государство
void racesSection(const World& w, const rules::FactionCalc& fc) {
  int n = int(fc.races.size());
  if (ui::Section s("Расы", "race", {.badge = n ? std::to_string(n) : std::string()}); s) {
    if (n == 0 || fc.population <= 0) {
      ui::emptyState("race", "Население провинций не указано.");
      return;
    }
    std::vector<std::string> names;
    std::vector<Color> colors;
    for (auto& [race, pop] : fc.races) {
      const CatalogItem* c = Catalogs::find(w.catalogs->races, race);
      names.push_back(c ? (c->name.empty() ? std::string("Без названия") : c->name) : std::string("Неизвестная раса"));
      colors.push_back(c ? c->color : Color::hex(0x8a8f99));
    }
    std::vector<ui::Slice> slices;
    for (size_t i = 0; i < names.size(); i++) slices.push_back({double(fc.races[i].second), colors[i], names[i]});
    // В центре — доля самой многочисленной расы.
    std::string center = fmtPct(double(fc.races[0].second) * 100.0 / double(fc.population));
    std::string lead = utf8::lower(names[0]);
    ui::pie(slices, {.size = 112, .thickness = 16, .centerValue = center, .centerLabel = lead, .legend = true});
    ui::label("Всего жителей: " + fmtNum(double(fc.population)), {.font = ui::Font::Small, .ink = ui::Ink::Muted});
    ui::spacer(4);
    ui::Column cols[] = {{"Раса", nullptr, ui::fr(1.1f, 90), ui::Align::Left, true},
                         {"Жители", nullptr, ui::px(78), ui::Align::Right, true},
                         {"Доля", nullptr, ui::fr(1, 90), ui::Align::Left, true}};
    ui::Table t("races", cols, n, {.rowHeight = 32, .selectable = false});
    t.sort([&](int a, int b, int col) {
      if (col == 0) return compareRu(names[size_t(a)], names[size_t(b)]);
      i64 x = fc.races[size_t(a)].second, y = fc.races[size_t(b)].second;
      return x < y ? -1 : x > y ? 1 : 0;
    });
    for (int i : t) {
      t.cell();
      {
        ui::Row rr({ui::px(10), ui::fr(1)}, 20, 8);
        RectF d = ui::next(10, 20);
        ui::draw::circle(d.cx(), d.cy(), 4.5f, colors[size_t(i)]);
        ui::label(names[size_t(i)]);
      }
      t.text(fmtNum(double(fc.races[size_t(i)].second)));
      t.cell();
      ui::progress(double(fc.races[size_t(i)].second) / double(fc.population), {.color = colors[size_t(i)], .label = true});
    }
  }
}

void drawProvinces(App& a, Id id) {
  const World& w = frameWorld(a);
  const Faction* f = w.faction(id);
  if (!f) return;
  auto calc = rules::calc(w);
  const rules::FactionCalc* fc = calc->faction(id);
  if (!fc) return;
  std::vector<const Province*> list;
  for (Id pid : fc->provinces)
    if (const Province* p = w.province(pid)) list.push_back(p);
  double trade = 0, prod = 0;
  for (const Province* p : list)
    if (const rules::ProvinceCalc* pc = calc->province(p->id)) {
      trade += pc->tradeValue;
      prod += pc->production;
    }
  {
    ui::Row r({ui::fr(1), ui::fr(1)}, 64, 10);
    ui::stat(fmtInt(i64(list.size())), plural(i64(list.size()), "провинция", "провинции", "провинций"), {.icon = "province", .tone = ui::Tone::Info});
    ui::stat(fmtShort(double(fc->population)), "Население", {.icon = "population", .tone = ui::Tone::Accent});
    ui::stat(fmtNum(trade), "Торговля", {.icon = "trade-value", .tone = ui::Tone::Success, .tooltip = "Сумма текущей торговой ценности провинций"});
    ui::stat(fmtNum(prod), "Добыча за ход", {.icon = "factory", .tone = ui::Tone::Warning, .tooltip = "Сумма добычи ресурсов провинций за ход"});
  }
  ui::spacer(2);
  if (ui::Section s("Провинции", "province", {.badge = list.empty() ? std::string() : std::to_string(list.size())}); s) {
    if (list.empty()) {
      if (ui::emptyState("province", "У государства пока нет провинций — назначьте владельца в панели провинции.", "Вся карта", "zoom-fit"))
        runCommand(a, "map.fit");
    } else {
      ui::Column cols[] = {{"Провинция · лорд", nullptr, ui::fr(1, 120), ui::Align::Left, true},
                           {{}, "resource", ui::px(64), ui::Align::Left, true, "Ресурс и добыча за ход"},
                           {{}, "population", ui::px(66), ui::Align::Right, true, "Население"},
                           {{}, "trade-value", ui::px(52), ui::Align::Right, true, "Торговая ценность"}};
      ui::Table t("provinces", cols, int(list.size()), {.rowHeight = 46});
      auto pcOf = [&](int i) { return calc->province(list[size_t(i)]->id); };
      t.sort([&](int x, int y, int col) {
        const Province& A = *list[size_t(x)];
        const Province& B = *list[size_t(y)];
        auto num = [](double u, double v) { return u < v ? -1 : u > v ? 1 : 0; };
        const rules::ProvinceCalc* pa = pcOf(x);
        const rules::ProvinceCalc* pb = pcOf(y);
        switch (col) {
          case 1: return num(pa ? pa->production : 0, pb ? pb->production : 0);
          case 2: return num(double(pa ? pa->population : 0), double(pb ? pb->population : 0));
          case 3: return num(pa ? pa->tradeValue : 0, pb ? pb->tradeValue : 0);
          default: return compareRu(provName(A), provName(B));
        }
      });
      for (int i : t) {
        const Province& p = *list[size_t(i)];
        const rules::ProvinceCalc* pc = pcOf(i);
        t.cell();
        {
          ui::Group g(0, 0);
          const char* icon = f->capital == p.id ? "capital" : p.occupied ? "occupied" : nullptr;
          std::string tip = f->capital == p.id ? std::string("Столица") : p.occupied ? "Оккупирована: " + w.factionName(p.occupier) : std::string();
          ui::label(provName(p), {.icon = icon, .tooltip = tip});
          const Character* lord = w.character(p.lord);
          ui::label(lord ? (lord->name.empty() ? std::string("Без имени") : lord->name) : std::string("Лорд не назначен"),
                    {.font = ui::Font::Small, .ink = lord ? ui::Ink::Dim : ui::Ink::Muted});
        }
        RectF rc = t.cell();
        ui::at(RectF{rc.x, std::round(rc.cy() - 11), rc.w, 22});
        if (p.resource) {
          ui::Row rr({ui::px(16), ui::fr(1)}, 22, 5);
          ui::iconColored(w::resourceIcon(w, p.resource), w::resourceColor(w, p.resource), 15, resourceName(w, p.resource));
          ui::label(fmtNum(pc ? pc->production : 0), {.font = ui::Font::Small, .ink = ui::Ink::Dim});
        } else {
          ui::label("—", {.ink = ui::Ink::Muted});
        }
        t.text(fmtShort(double(pc ? pc->population : 0)), ui::Ink::Dim, ui::Font::Small);
        t.text(fmtNum(pc ? pc->tradeValue : 0), ui::Ink::Dim, ui::Font::Small);
      }
      if (int c = t.clicked(); c >= 0) a.select(SelType::Province, list[size_t(c)]->id, true);
      a.markUi("provinces.table");
    }
  }
  racesSection(w, *fc);
}

// ---------------------------------------------------------------- гильдия
// Провинция для нового штаба: сухопутные провинции без штаба этой гильдии; где уже 5 штабов — недоступны.
void hqPicker(const World& w, Id guild, Id& value) {
  std::vector<const Province*> list;
  w.provinces.each([&](const Province& p) {
    if (p.sea || std::find(p.hqs.begin(), p.hqs.end(), guild) != p.hqs.end()) return;
    list.push_back(&p);
  });
  std::sort(list.begin(), list.end(), [](const Province* x, const Province* y) { return compareRu(provName(*x), provName(*y)) < 0; });
  std::vector<std::string> labels, hints;
  labels.reserve(list.size());
  hints.reserve(list.size());
  for (const Province* p : list) {
    labels.push_back(provName(*p));
    hints.push_back(std::to_string(p->hqs.size()) + "/" + std::to_string(schema::kMaxHqPerProvince));
  }
  std::vector<ui::Option> opts(list.size());
  int idx = -1;
  for (size_t i = 0; i < list.size(); i++) {
    opts[i].label = labels[i];
    opts[i].icon = "province";
    if (list[i]->owner) opts[i].color = w::factionColor(w, list[i]->owner);
    opts[i].hint = hints[i];
    opts[i].disabled = int(list[i]->hqs.size()) >= schema::kMaxHqPerProvince;
    if (list[i]->id == value) idx = int(i);
  }
  ui::ComboOpt co;
  co.placeholder = "Провинция для штаба";
  co.icon = "province";
  co.search = 1;
  co.tooltip = "Справа — штабов в провинции из 5";
  if (ui::combo("hqpick", idx, std::span<const ui::Option>(opts), co) && idx >= 0 && idx < int(list.size())) value = list[size_t(idx)]->id;
}

void drawHqs(App& a, Id id) {
  const World& w = frameWorld(a);
  const Faction* f = w.faction(id);
  if (!f) return;
  const bool ro = a.readOnly();
  auto calc = rules::calc(w);
  const rules::FactionCalc* fc = calc->faction(id);
  if (!fc) return;
  struct HqRow {
    const Province* p;
    rules::GuildShare share;
  };
  std::vector<HqRow> rows;
  double gross = 0, tax = 0;
  for (Id pid : fc->provinces) {
    const Province* p = w.province(pid);
    if (!p) continue;
    HqRow r{p, {}};
    r.share.guild = id;
    if (const rules::ProvinceCalc* pc = calc->province(pid))
      for (const rules::GuildShare& s : pc->guilds)
        if (s.guild == id) r.share = s;
    gross += r.share.gross;
    tax += r.share.tax;
    rows.push_back(r);
  }
  {
    ui::Row r({ui::fr(1), ui::fr(1)}, 64, 10);
    ui::stat(fmtInt(i64(rows.size())), plural(i64(rows.size()), "штаб", "штаба", "штабов"),
             {.icon = "hq", .tone = ui::Tone::Info, .tooltip = "В провинции — не больше 5 штабов разных гильдий, у гильдии — один штаб в провинции"});
    ui::stat(moneySigned(fc->incGuilds), "Доход штабов", {.icon = "income", .tone = ui::Tone::Success,
                                                           .tooltip = "Σ торговая ценность × влияние гильдии − налог провинции"});
  }
  ui::spacer(2);
  if (ui::Section s("Штабы", "hq", {.badge = rows.empty() ? std::string() : std::to_string(rows.size())}); s) {
    if (rows.empty()) {
      ui::emptyState("hq", ro ? "Штабов нет." : "Штабов пока нет — выберите провинцию ниже.");
    } else {
      {  // таблица закрывается до подписи под ней
        ui::Column cols[] = {{"Провинция", nullptr, ui::fr(1, 100), ui::Align::Left, true},
                             {"Влияние", nullptr, ui::px(84), ui::Align::Left, true, "Торговое влияние гильдии в провинции, %"},
                             {{}, "income", ui::px(58), ui::Align::Right, true, "Чистый доход штаба за ход (после налога)"},
                             {{}, nullptr, ui::px(44)}};
        ui::Table t("hqs", cols, int(rows.size()), {.rowHeight = 40});
        t.sort([&](int x, int y, int col) {
          auto num = [](double u, double v) { return u < v ? -1 : u > v ? 1 : 0; };
          const HqRow& A = rows[size_t(x)];
          const HqRow& B = rows[size_t(y)];
          if (col == 1) return num(A.share.pct, B.share.pct);
          if (col == 2) return num(A.share.net, B.share.net);
          return compareRu(provName(*A.p), provName(*B.p));
        });
        for (int i : t) {
          const HqRow& r = rows[size_t(i)];
          const Id pid = r.p->id;
          ui::IdScope sc{i64(pid)};
          t.cell();
          {
            ui::Group g(0, 0);
            ui::label(provName(*r.p), {.tooltip = "Штабов в провинции: " + std::to_string(r.p->hqs.size()) + " из " + std::to_string(schema::kMaxHqPerProvince)});
            ui::Row rr({ui::px(8), ui::fr(1)}, 16, 5);
            RectF d = ui::next(8, 16);
            if (r.p->owner) ui::draw::circle(d.cx(), d.cy(), 3.5f, w::factionColor(w, r.p->owner));
            ui::label(r.p->owner ? w.factionName(r.p->owner) : std::string("Без владельца"), {.font = ui::Font::Small, .ink = ui::Ink::Muted});
          }
          t.cell();
          double pct = r.share.pct;
          if (ui::numberField("inf", pct, {.min = 0, .max = 100, .step = 1, .digits = 1, .unit = "%", .disabled = ro,
                                           .tooltip = "Торговое влияние гильдии; сумма по провинции — не больше 100 %"}))
            a.act("Влияние гильдии", [&](Tx& tx) { rules::setInfluence(tx, pid, id, pct); },
                  {.coalesce = "influence:" + std::to_string(pid) + ":" + std::to_string(id)});
          a.markUi("hq.influence." + std::to_string(pid));
          t.text(money(r.share.net), r.share.net > 0 ? ui::Ink::Success : ui::Ink::Muted);
          t.cell();
          if (ui::iconButton("trash", "Закрыть штаб", {.size = ui::Size::Small, .disabled = ro, .tone = ui::Tone::Danger})) {
            std::string pname = provName(*r.p);
            a.confirm("Закрыть штаб в провинции «" + pname + "»?", "Гильдия перестанет получать доход от этой провинции. Отменить можно сочетанием Ctrl+Z.",
                      "Закрыть штаб", true, [id, pid](App& x) { x.act("Закрыть штаб", [&](Tx& tx) { rules::removeHq(tx, id, pid); }); });
          }
          a.markUi("hq.remove." + std::to_string(i));
        }
        if (t.footer()) {
          t.text("Итого");
          t.text({});
          t.text(money(fc->incGuilds));
          t.text({});
        }
        if (int c = t.clicked(); c >= 0) a.select(SelType::Province, rows[size_t(c)].p->id, true);
      }
      ui::label("Валовой доход " + money(gross) + " · налог провинциям " + money(tax), {.font = ui::Font::Small, .ink = ui::Ink::Muted,
                                                                                         .tooltip = "Налог штабов идёт владельцам провинций"});
    }
    // Открыть штаб: провинция без штаба этой гильдии и кнопка.
    if (!ro) {
      ui::spacer(2);
      Id& pick = ui::state<Id>(ui::id("##hqpick"));
      if (pick && (!w.province(pick) || std::find(fc->provinces.begin(), fc->provinces.end(), pick) != fc->provinces.end())) pick = 0;
      ui::Row r({ui::fr(1), ui::px(136)}, 30, 8);
      hqPicker(w, id, pick);
      a.markUi("hq.pick");
      if (ui::button("Открыть штаб", {.icon = "plus", .fill = true, .disabled = pick == 0, .tooltip = "Штаб в выбранной провинции"})) {
        Id pid = pick;
        if (a.act("Штаб гильдии", [&](Tx& tx) { rules::buildHq(tx, id, pid); })) pick = 0;
      }
      a.markUi("hq.add");
    }
  }
}

bool stateOnly(App& a, Id id) { return isState(a, id); }
bool guildOnly(App& a, Id id) { return isGuild(a, id); }

TabReg tabProvinces({kTabProvinces, "province", "Провинции", 20, SelType::Faction, stateOnly, drawProvinces});
TabReg tabHqs({kTabHqs, "hq", "Штабы", 20, SelType::Faction, guildOnly, drawHqs});

}  // namespace
}  // namespace rg::app
