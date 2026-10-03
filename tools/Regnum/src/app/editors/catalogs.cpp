// Regnum — окно справочников: ресурсы, расы, культуры, религии, формы правления, должности.
// Таблицы с правкой названия, цвета и значка (ресурсы), добавлением, удалением через rules::removeCatalogItem
// (с предупреждением о местах использования; «Золото» и встроенные записи закреплены), числом использований
// и карточкой выбранной записи со ссылками. Быстрый доступ — выдвижная панель «Справочники» и команда палитры.
#include <algorithm>

#include "app/app_internal.h"
#include "app/widgets.h"
#include "gfx/icons.h"

namespace rg::app {

namespace edkit {   // modifiers.cpp
bool iconPicker(std::string_view id, std::string& icon, bool allowNone, bool disabled, std::string_view tip);
const char* iconTitle(std::string_view icon);
void iconTile(const std::string& icon, Color tint, float size, const char* fallback);
bool entityRow(std::string_view title, std::string_view subtitle, const char* icon, Color tint, std::string_view hint, bool selected,
               bool warn, std::string_view tip);
void goTo(App& a, Selection s);
void chipsBegin();
ui::ChipAction chip(std::string_view label, const ui::ChipOpt& o);
void chipsEnd();
}  // namespace edkit

namespace {

using platform::Key;
using rules::CatalogList;

// Фишки с переносом строк.
struct ChipFlow {
  ChipFlow() { edkit::chipsBegin(); }
  ~ChipFlow() { edkit::chipsEnd(); }
  ChipFlow(const ChipFlow&) = delete;
  ChipFlow& operator=(const ChipFlow&) = delete;
};

std::string orName(const std::string& s, const char* fallback) { return s.empty() ? std::string(fallback) : s; }
std::string nb(i64 n, const char* one, const char* few, const char* many) { return fmtInt(n) + "\xC2\xA0" + plural(n, one, few, many); }

// ---------------------------------------------------------------- описание справочников
struct ListDef {
  CatalogList list;
  const char* icon;
  const char* title;     // вкладка
  const char* noun;      // «Ресурс»
  const char* newLabel;  // кнопка добавления
  bool colored;          // цвет используется на карте и в фишках
};
const ListDef kLists[] = {
    {CatalogList::Resources, "resource", "Ресурсы", "Ресурс", "Новый ресурс", true},
    {CatalogList::Races, "race", "Расы", "Раса", "Новая раса", true},
    {CatalogList::Cultures, "culture", "Культуры", "Культура", "Новая культура", true},
    {CatalogList::Religions, "religion", "Религии", "Религия", "Новая религия", true},
    {CatalogList::Governments, "crown", "Формы правления", "Форма правления", "Новая форма правления", false},
    {CatalogList::Positions, "council", "Должности", "Должность", "Новая должность", false},
};
constexpr int kListCount = int(std::size(kLists));

bool locked(CatalogList l, const CatalogItem& c) { return c.builtin || (l == CatalogList::Resources && c.id == kGold); }

// ---------------------------------------------------------------- использование записи
struct CatUse {
  std::vector<Id> provinces;               // ресурс, раса, культура, религия
  std::vector<Id> factions;                // культура, религия, форма правления; ресурс — запасы
  std::vector<Id> buildings;               // ресурс — стоимость уровней
  std::vector<Id> deals;                   // ресурс — сделки
  std::vector<std::pair<Id, Id>> seats;    // должность: фракция, место в совете
  i64 population = 0;                      // раса
  double stock = 0, production = 0;        // ресурс: запасы фракций, добыча провинций за ход
  size_t total() const { return provinces.size() + factions.size() + buildings.size() + deals.size() + seats.size(); }
};

CatUse usageOf(const World& w, CatalogList l, const CatalogItem& it, const rules::Calc* calc) {
  CatUse u;
  Id id = it.id;
  switch (l) {
    case CatalogList::Resources:
      w.provinces.each([&](const Province& p) {
        if (p.sea || p.resource != id) return;
        u.provinces.push_back(p.id);
        if (calc)
          if (const rules::ProvinceCalc* pc = calc->province(p.id)) u.production += pc->production;
      });
      w.factions.each([&](const Faction& f) {
        auto r = f.res.find(id);
        if (r == f.res.end() || r->second == 0) return;
        u.factions.push_back(f.id);
        u.stock += r->second;
      });
      w.buildings.each([&](const Building& b) {
        for (auto& lv : b.levels)
          if (lv.cost.count(id)) {
            u.buildings.push_back(b.id);
            break;
          }
      });
      w.deals.each([&](const Deal& d) {
        if (d.status != DealStatus::Active) return;
        for (auto& x : d.items)
          if (x.res == id) {
            u.deals.push_back(d.id);
            break;
          }
      });
      break;
    case CatalogList::Races:
      w.provinces.each([&](const Province& p) {
        if (p.sea) return;
        i64 pop = 0;
        bool any = false;
        for (auto& r : p.races)
          if (r.race == id) {
            any = true;
            pop += r.pop;
          }
        if (any) {
          u.provinces.push_back(p.id);
          u.population += pop;
        }
      });
      break;
    case CatalogList::Cultures:
    case CatalogList::Religions: {
      bool cul = l == CatalogList::Cultures;
      w.provinces.each([&](const Province& p) {
        if (!p.sea && (cul ? p.culture : p.religion) == id) u.provinces.push_back(p.id);
      });
      w.factions.each([&](const Faction& f) {
        if ((cul ? f.culture : f.religion) == id) u.factions.push_back(f.id);
      });
      break;
    }
    case CatalogList::Governments:
      w.factions.each([&](const Faction& f) {
        if (f.government == id) u.factions.push_back(f.id);
      });
      break;
    case CatalogList::Positions: {
      std::string key = utf8::searchKey(it.name);
      w.factions.each([&](const Faction& f) {
        for (auto& s : f.council)
          if (utf8::searchKey(s.position) == key) u.seats.push_back({f.id, s.id});
      });
      break;
    }
  }
  std::sort(u.provinces.begin(), u.provinces.end(), [&](Id x, Id y) { return compareRu(w.provinceName(x), w.provinceName(y)) < 0; });
  std::sort(u.factions.begin(), u.factions.end(), [&](Id x, Id y) { return compareRu(w.factionName(x), w.factionName(y)) < 0; });
  return u;
}

std::string usageText(CatalogList l, const CatUse& u) {
  std::vector<std::string> parts;
  if (!u.provinces.empty()) parts.push_back(nb(i64(u.provinces.size()), "провинция", "провинции", "провинций"));
  if (!u.factions.empty()) {
    if (l == CatalogList::Resources) parts.push_back("запасы " + nb(i64(u.factions.size()), "фракции", "фракций", "фракций"));
    else parts.push_back(nb(i64(u.factions.size()), "государство", "государства", "государств"));
  }
  if (!u.buildings.empty()) parts.push_back("стоимость " + nb(i64(u.buildings.size()), "постройки", "построек", "построек"));
  if (!u.deals.empty()) parts.push_back(nb(i64(u.deals.size()), "сделка", "сделки", "сделок"));
  if (!u.seats.empty()) parts.push_back(nb(i64(u.seats.size()), "место в совете", "места в совете", "мест в совете"));
  std::string s;
  for (size_t i = 0; i < parts.size(); i++) s += (i ? ", " : "") + parts[i];
  return s;
}

// ---------------------------------------------------------------- действия
Id addItem(App& a, CatalogList l) {
  Id nid = 0;
  a.act("Добавить в справочник", [&](Tx& tx) { nid = rules::addCatalogItem(tx, l, ""); });
  return nid;
}

void renameItem(App& a, const ListDef& d, Id id, const std::string& raw) {
  std::string name = trim(raw);
  CatalogList l = d.list;
  std::string label = std::string("Переименовать: ") + d.noun;
  a.act(label, [&](Tx& tx) {
    if (name.empty()) fail("Название не может быть пустым");
    auto& items = rules::catalogList(tx.catalogs(), l);
    std::string key = utf8::searchKey(name), old;
    for (auto& c : items)
      if (c.id != id && utf8::searchKey(c.name) == key) fail(std::string(d.noun) + " «" + c.name + "» уже есть в справочнике");
    for (auto& c : items)
      if (c.id == id) {
        old = c.name;
        c.name = name;
      }
    // Должность хранится в местах совета текстом — переименовать и там.
    if (l == CatalogList::Positions && !old.empty()) {
      std::string ok = utf8::searchKey(old);
      std::vector<Id> fids;
      tx.w().factions.each([&](const Faction& f) {
        for (auto& s : f.council)
          if (utf8::searchKey(s.position) == ok) {
            fids.push_back(f.id);
            break;
          }
      });
      for (Id fid : fids)
        for (auto& s : tx.faction(fid).council)
          if (utf8::searchKey(s.position) == ok) s.position = name;
    }
  });
}

void askRemove(App& a, const ListDef& d, Id id) {
  const World& w = a.world();
  const CatalogItem* it = Catalogs::find(rules::catalogList(*w.catalogs, d.list), id);
  if (!it) return;
  if (locked(d.list, *it)) {
    a.toast("«" + it->name + "» — встроенная запись, её нельзя удалить", ToastKind::Warning, "lock");
    return;
  }
  auto calc = rules::calc(w);
  CatUse u = usageOf(w, d.list, *it, calc.get());
  std::string name = "«" + orName(it->name, "Без названия") + "»";
  std::string text;
  if (u.total()) {
    text = name + " используется: " + usageText(d.list, u) + ".";
    if (d.list == CatalogList::Positions) text += " Места в совете останутся с прежним названием должности.";
    else text += " Все ссылки будут очищены.";
  } else {
    text = name + " нигде не используется.";
  }
  text += " Действие можно отменить Ctrl+Z.";
  CatalogList l = d.list;
  std::string title = std::string("Удалить: ") + d.noun + "?";
  a.confirm(title, text, "Удалить", true, [l, id](App& x) {
    x.act("Удалить из справочника", [&](Tx& tx) { rules::removeCatalogItem(tx, l, id); });
  });
}

// ---------------------------------------------------------------- состояние окна
struct EdState {
  int tab = 0;
  std::string query;
  std::array<Id, kListCount> sel{};
  bool focusSearch = false;
  Id focusName = 0;     // запись, чьё название получить фокус (после добавления)
};

// Образец цвета и значка записи.
void swatch(const ListDef& d, const CatalogItem& c, float size) {
  const ui::Theme& th = ui::theme();
  RectF r = ui::next(size, size);
  RectF t{r.x, r.cy() - size * 0.5f, size, size};
  if (d.list == CatalogList::Resources) {
    ui::draw::rect(t, c.color.alpha(th.dark ? 0.22f : 0.18f), 7);
    ui::draw::icon(c.id == kGold ? "coins" : (!c.icon.empty() && gfx::hasIcon(c.icon) ? std::string_view(c.icon) : std::string_view("resource")),
                   t.inset(size * 0.2f), c.color);
  } else if (d.colored) {
    ui::draw::rect(t, c.color.alpha(th.dark ? 0.22f : 0.18f), 7);
    ui::draw::circle(t.cx(), t.cy(), size * 0.22f, c.color);
  } else {
    ui::draw::rect(t, th.surface3, 7);
    ui::draw::icon(d.icon, t.inset(size * 0.22f), th.textDim);
  }
}

// ---------------------------------------------------------------- таблица справочника
struct Row {
  const CatalogItem* item;
  CatUse use;
};

void drawTable(App& a, EdState& st, const ListDef& d, std::vector<Row>& rows, float height) {
  bool ro = a.readOnly();
  bool res = d.list == CatalogList::Resources;
  std::vector<ui::Column> cols;
  cols.push_back({"", nullptr, ui::px(38)});
  cols.push_back({"Название", nullptr, ui::fr(2, 160), ui::Align::Left, true});
  if (d.colored) cols.push_back({"Цвет", "palette", ui::px(136)});
  if (res) cols.push_back({"", "image", ui::px(46), ui::Align::Left, false, "Значок ресурса"});
  switch (d.list) {
    case CatalogList::Resources:
      cols.push_back({"Провинции", "province", ui::fr(1, 96), ui::Align::Right, true, "Провинции, добывающие ресурс"});
      cols.push_back({"Добыча", "pickaxe", ui::fr(1, 96), ui::Align::Right, true, "Добыча всех провинций за ход"});
      cols.push_back({"Запасы", "treasury", ui::fr(1, 104), ui::Align::Right, true, "Сумма запасов всех фракций"});
      break;
    case CatalogList::Races:
      cols.push_back({"Провинции", "province", ui::fr(1, 96), ui::Align::Right, true, "Провинции, где живёт раса"});
      cols.push_back({"Население", "population", ui::fr(1, 110), ui::Align::Right, true});
      break;
    case CatalogList::Cultures:
    case CatalogList::Religions:
      cols.push_back({"Провинции", "province", ui::fr(1, 96), ui::Align::Right, true});
      cols.push_back({"Государства", "crown", ui::fr(1, 110), ui::Align::Right, true, "Государства, где это основная запись"});
      break;
    case CatalogList::Governments:
      cols.push_back({"Государства", "crown", ui::fr(1, 110), ui::Align::Right, true});
      break;
    case CatalogList::Positions:
      cols.push_back({"Места в совете", "council", ui::fr(1, 130), ui::Align::Right, true});
      break;
  }
  cols.push_back({"", nullptr, ui::px(40)});
  int selIdx = -1;
  for (size_t i = 0; i < rows.size(); i++)
    if (rows[i].item->id == st.sel[size_t(st.tab)]) selIdx = int(i);
  int selBefore = selIdx;
  std::string emptyText = st.query.empty() ? std::string("Справочник пуст") : std::string("Ничего не найдено");
  ui::Table t("tbl", cols, int(rows.size()),
              {.rowHeight = 42, .height = height, .selected = &selIdx, .emptyIcon = d.icon, .emptyText = emptyText});
  // Номер столбца с числом: 0 — название, далее по порядку числовых столбцов.
  int firstNum = 2 + (d.colored ? 1 : 0) + (res ? 1 : 0);
  t.sort([&](int x, int y, int col) {
    const Row& A = rows[size_t(x)];
    const Row& B = rows[size_t(y)];
    auto cmpd = [](double p, double q) { return p < q ? -1 : p > q ? 1 : 0; };
    if (col == 1) return compareRu(A.item->name, B.item->name);
    int k = col - firstNum;
    switch (d.list) {
      case CatalogList::Resources:
        if (k == 0) return cmpd(double(A.use.provinces.size()), double(B.use.provinces.size()));
        if (k == 1) return cmpd(A.use.production, B.use.production);
        return cmpd(A.use.stock, B.use.stock);
      case CatalogList::Races:
        if (k == 0) return cmpd(double(A.use.provinces.size()), double(B.use.provinces.size()));
        return cmpd(double(A.use.population), double(B.use.population));
      case CatalogList::Cultures:
      case CatalogList::Religions:
        if (k == 0) return cmpd(double(A.use.provinces.size()), double(B.use.provinces.size()));
        return cmpd(double(A.use.factions.size()), double(B.use.factions.size()));
      case CatalogList::Governments: return cmpd(double(A.use.factions.size()), double(B.use.factions.size()));
      case CatalogList::Positions: return cmpd(double(A.use.seats.size()), double(B.use.seats.size()));
    }
    return 0;
  });
  for (int i : t) {
    const CatalogItem& c = *rows[size_t(i)].item;
    const CatUse& u = rows[size_t(i)].use;
    Id id = c.id;
    t.cell();
    swatch(d, c, 28);
    t.cell();
    {
      std::string name = c.name;
      if (st.focusName == id) {
        ui::setKeyboardFocus(ui::id("name"));
        ui::scrollToItem();
        st.focusName = 0;
      }
      if (ui::textField("name", name, {.placeholder = "Название", .maxLength = 60, .readOnly = ro, .selectAllOnFocus = true}) && name != c.name)
        renameItem(a, d, id, name);
      if (ui::lastItem().focused && id != st.sel[size_t(st.tab)]) st.sel[size_t(st.tab)] = id;   // правка строки — её карточка
      if (id == st.sel[size_t(st.tab)]) a.markUi("catalogs.selected.name");
    }
    if (d.colored) {
      t.cell();
      Color col = c.color;
      ui::Disabled dcol(ro);
      if (ui::colorButton("color", col, {.tooltip = "Цвет на карте и в списках", .size = ui::Size::Small}) && !ro) {
        CatalogList l = d.list;
        a.act("Цвет записи справочника", [&](Tx& tx) {
          for (auto& x : rules::catalogList(tx.catalogs(), l))
            if (x.id == id) x.color = col;
        }, {.coalesce = "catcolor:" + std::to_string(int(l)) + ":" + std::to_string(id)});
      }
    }
    if (res) {
      t.cell();
      if (id == kGold) {
        ui::icon("coins", ui::Ink::Accent, 18, "Значок казны закреплён");
      } else {
        std::string icon = c.icon;
        if (edkit::iconPicker("icon", icon, true, ro, "Значок ресурса") && icon != c.icon)
          a.act("Значок ресурса", [&](Tx& tx) {
            for (auto& x : tx.catalogs().resources)
              if (x.id == id) x.icon = icon;
          });
      }
    }
    auto num = [&](double v, bool zeroDim = true) { t.text(v == 0 && zeroDim ? std::string("—") : fmtNum(v), v == 0 ? ui::Ink::Muted : ui::Ink::Normal); };
    switch (d.list) {
      case CatalogList::Resources:
        num(double(u.provinces.size()));
        num(std::round(u.production * 10) / 10);
        if (id == kGold) t.text(fmtNum(u.stock), ui::Ink::Accent);
        else num(u.stock);
        break;
      case CatalogList::Races:
        num(double(u.provinces.size()));
        t.text(u.population ? fmtShort(double(u.population)) : std::string("—"), u.population ? ui::Ink::Normal : ui::Ink::Muted);
        break;
      case CatalogList::Cultures:
      case CatalogList::Religions:
        num(double(u.provinces.size()));
        num(double(u.factions.size()));
        break;
      case CatalogList::Governments: num(double(u.factions.size())); break;
      case CatalogList::Positions: num(double(u.seats.size())); break;
    }
    t.cell();
    if (locked(d.list, c)) {
      ui::icon("lock", ui::Ink::Muted, 16, "Встроенная запись — удалить нельзя");
    } else if (ui::iconButton("trash", "Удалить запись", {.size = ui::Size::Small, .disabled = ro, .tone = ui::Tone::Danger})) {
      askRemove(a, d, id);
    }
  }
  if (selIdx != selBefore && selIdx >= 0 && selIdx < int(rows.size())) st.sel[size_t(st.tab)] = rows[size_t(selIdx)].item->id;
}

// ---------------------------------------------------------------- карточка записи
void drawCard(App& a, EdState& st, const ListDef& d, const Row& row) {
  const World& w = a.world();
  const CatalogItem& c = *row.item;
  const CatUse& u = row.use;
  bool ro = a.readOnly();
  ui::Scroll sc("card");
  {
    ui::Row head({ui::px(52), ui::fr(1)}, 52, 12);
    {
      RectF r = ui::next(52, 52);
      const ui::Theme& th = ui::theme();
      Color tint = d.colored ? c.color : th.textDim;
      ui::draw::rect(r, tint.alpha(th.dark ? 0.2f : 0.16f), 12);
      ui::draw::rectStroke(r, tint.alpha(0.45f), 12, 1);
      const char* ic = d.icon;
      if (d.list == CatalogList::Resources) ic = w::resourceIcon(w, c.id);
      ui::draw::icon(ic, r.inset(13), tint);
    }
    {
      ui::Group g(0, 2);
      ui::caption(d.noun);
      ui::label(orName(c.name, "Без названия"), {.font = ui::Font::Title});
    }
  }
  if (locked(d.list, c)) {
    ui::HStack hs(24, ui::Align::Left, 6);
    ui::tag(c.id == kGold && d.list == CatalogList::Resources ? "Казна государства" : "Встроенная запись", ui::Tone::Accent, "lock");
  }
  // Показатели записи.
  {
    ui::Row tiles({ui::fr(1), ui::fr(1)}, 60, 8);
    switch (d.list) {
      case CatalogList::Resources:
        if (c.id == kGold) {   // золото — казна: добычи провинций нет
          ui::stat(fmtShort(u.stock), "Казна всех фракций", {.icon = "treasury", .tone = ui::Tone::Accent});
          ui::stat(fmtInt(i64(u.factions.size())), "Фракции с казной", {.icon = "crown", .tone = ui::Tone::Info});
          break;
        }
        ui::stat(fmtNum(std::round(u.production * 10) / 10, 1), "Добыча за ход", {.icon = "pickaxe", .tone = ui::Tone::Success});
        ui::stat(fmtShort(u.stock), "Запасы фракций", {.icon = "treasury", .tone = ui::Tone::Accent});
        break;
      case CatalogList::Races:
        ui::stat(fmtShort(double(u.population)), "Население", {.icon = "population", .tone = ui::Tone::Info});
        ui::stat(fmtInt(i64(u.provinces.size())), "Провинции", {.icon = "province", .tone = ui::Tone::Accent});
        break;
      case CatalogList::Cultures:
      case CatalogList::Religions:
        ui::stat(fmtInt(i64(u.provinces.size())), "Провинции", {.icon = "province", .tone = ui::Tone::Info});
        ui::stat(fmtInt(i64(u.factions.size())), "Государства", {.icon = "crown", .tone = ui::Tone::Accent});
        break;
      case CatalogList::Governments:
        ui::stat(fmtInt(i64(u.factions.size())), "Государства", {.icon = "crown", .tone = ui::Tone::Accent});
        ui::stat(fmtInt(i64(u.total())), "Всего ссылок", {.icon = "link", .tone = ui::Tone::Info});
        break;
      case CatalogList::Positions:
        ui::stat(fmtInt(i64(u.seats.size())), "Места в совете", {.icon = "council", .tone = ui::Tone::Accent});
        ui::stat(fmtInt(i64(std::count_if(u.seats.begin(), u.seats.end(), [&](auto& s) {
                   const Faction* f = w.faction(s.first);
                   if (!f) return false;
                   for (auto& x : f->council)
                     if (x.id == s.second) return x.character == 0;
                   return false;
                 }))),
                 "Свободные", {.icon = "user", .tone = ui::Tone::Warning});
        break;
    }
  }
  // Где используется.
  {
    std::string badge = std::to_string(u.total());
    ui::Section sec("Где используется", "link", {.badge = badge});
    a.markUi("catalogs.usage");
    if (sec) {
      if (u.total() == 0) ui::label("Нигде не используется.", {.ink = ui::Ink::Muted});
      if (!u.provinces.empty()) {
        ui::caption("Провинции · " + std::to_string(u.provinces.size()));
        ChipFlow cf;
        for (Id pid : u.provinces) {
          const Province* p = w.province(pid);
          ui::IdScope s{i64(pid)};
          ui::ChipOpt co;
          co.icon = "province";
          co.color = w::factionColor(w, p ? p->owner : 0);
          co.clickable = true;
          co.tooltip = "Открыть провинцию";
          if (edkit::chip(orName(p ? p->name : std::string(), "Без названия"), co) == ui::ChipAction::Click) edkit::goTo(a, {SelType::Province, pid});
        }
      }
      if (!u.factions.empty()) {
        ui::caption((d.list == CatalogList::Resources ? std::string("Запасы · ") : std::string("Государства · ")) + std::to_string(u.factions.size()));
        ChipFlow cf;
        for (Id fid : u.factions) {
          const Faction* f = w.faction(fid);
          ui::IdScope s{i64(fid) + 0x100000};
          ui::ChipOpt co;
          co.color = w::factionColor(w, fid);
          co.clickable = true;
          co.tooltip = f && f->isGuild() ? "Открыть гильдию" : "Открыть государство";
          std::string label = orName(f ? f->name : std::string(), "Без названия");
          if (d.list == CatalogList::Resources && f) label += " · " + fmtShort(f->stock(c.id));
          if (edkit::chip(label, co) == ui::ChipAction::Click) edkit::goTo(a, {SelType::Faction, fid});
        }
      }
      if (!u.seats.empty()) {
        ui::caption("Совет · " + std::to_string(u.seats.size()));
        ChipFlow cf;
        for (auto [fid, sid] : u.seats) {
          const Faction* f = w.faction(fid);
          if (!f) continue;
          ui::IdScope s{i64(sid) + 0x200000};
          std::string who = "место свободно";
          for (auto& x : f->council)
            if (x.id == sid && x.character) who = w.characterName(x.character);
          ui::ChipOpt co;
          co.color = f->color;
          co.clickable = true;
          co.tooltip = "Открыть государство";
          if (edkit::chip(orName(f->name, "Без названия") + " · " + who, co) == ui::ChipAction::Click) edkit::goTo(a, {SelType::Faction, fid});
        }
      }
      if (!u.buildings.empty()) {
        ui::caption("Стоимость построек · " + std::to_string(u.buildings.size()));
        ChipFlow cf;
        for (Id bid : u.buildings) {
          const Building* b = w.building(bid);
          if (!b) continue;
          ui::IdScope s{i64(bid) + 0x300000};
          edkit::chip(orName(b->name, "Без названия"), {.icon = b->icon.empty() || !gfx::hasIcon(b->icon) ? "building" : b->icon.c_str(),
                                                     .color = b->owner ? w::factionColor(w, b->owner) : Color(0, 0, 0, 0)});
        }
      }
      if (!u.deals.empty()) {
        ui::caption("Действующие сделки · " + std::to_string(u.deals.size()));
        ChipFlow cf;
        for (Id did : u.deals) {
          const Deal* dl = w.deal(did);
          if (!dl) continue;
          ui::IdScope s{i64(did) + 0x400000};
          edkit::chip(w.factionName(dl->a) + " ⇄ " + w.factionName(dl->b), {.icon = "handshake"});
        }
      }
    }
  }
  ui::spacer(4);
  if (!locked(d.list, c)) {
    ui::Disabled dis(ro);
    if (ui::button(std::string("Удалить: ") + orName(c.name, "запись"), {.variant = ui::Variant::Danger, .icon = "trash", .fill = true}))
      askRemove(a, d, c.id);
    a.markUi("catalogs.delete");
  }
  (void)st;
}

// ---------------------------------------------------------------- окно
void drawEditor(App& a, Id arg) {
  const World& w = a.world();
  const ui::Theme& th = ui::theme();
  EdState& st = ui::state<EdState>(ui::id("##catstate"));
  if (arg >= 1 && arg <= Id(kListCount)) {
    st.tab = int(arg) - 1;
    a.ui.editorArg = 0;   // вкладка выбрана; дальше — по щелчкам
  }
  bool ro = a.readOnly();
  // Вкладки с числом записей.
  std::vector<ui::Tab> items;
  for (int i = 0; i < kListCount; i++) {
    int n = int(rules::catalogList(*w.catalogs, kLists[i].list).size());
    items.push_back(ui::Tab{kLists[i].icon, kLists[i].title, kLists[i].title, n, ui::Tone::Neutral});
  }
  int tab = st.tab;
  ui::tabs("tabs", tab, std::span<const ui::Tab>(items));
  a.markUi("catalogs.tabs");
  if (tab != st.tab) {
    st.tab = tab;
    st.query.clear();
  }
  const ListDef& d = kLists[st.tab];
  ui::spacer(4);
  // Панель: поиск и добавление.
  if (ui::shortcut({Key::F, ui::ModPrimary})) st.focusSearch = true;
  {
    ui::HStack hs(30, ui::Align::Left, 8);
    {
      RectF r = ui::next(300, 30);
      ui::at(r);
      if (st.focusSearch) {
        ui::setKeyboardFocus(ui::id("q"));
        st.focusSearch = false;
      }
      ui::searchField("q", st.query, "Поиск в справочнике");
      a.markUi("catalogs.search");
    }
    ui::flex();
    if (ui::button(d.newLabel, {.variant = ui::Variant::Primary, .icon = "plus", .disabled = ro, .shortcut = {Key::Insert, 0}})) {
      if (Id nid = addItem(a, d.list)) {
        st.query.clear();
        st.sel[size_t(st.tab)] = nid;
        st.focusName = nid;
      }
    }
    ui::tooltip(std::string(d.newLabel), {Key::Insert, 0});
    a.markUi("catalogs.add");
  }
  ui::spacer(2);
  // Строки (с фильтром) и использование.
  auto calc = rules::calc(w);
  const auto& list = rules::catalogList(*w.catalogs, d.list);
  std::vector<Row> rows;
  for (const CatalogItem& c : list) {
    if (!st.query.empty() && !utf8::matches(c.name, st.query)) continue;
    rows.push_back(Row{&c, usageOf(w, d.list, c, calc.get())});
  }
  Id& sel = st.sel[size_t(st.tab)];
  bool selVisible = false;
  for (auto& r : rows) selVisible = selVisible || r.item->id == sel;
  if (!selVisible) sel = rows.empty() ? 0 : rows.front().item->id;

  RectF R = ui::avail();
  float cardW = R.w >= 980 ? 360 : 0;
  RectF T{R.x, R.y, R.w - (cardW > 0 ? cardW + 24 : 0), R.h};
  {
    ui::Area ta(T, 0);
    drawTable(a, st, d, rows, std::max(120.f, T.h - 2));
  }
  a.markUi("catalogs.table", T);
  if (cardW > 0) {
    RectF C{T.right() + 24, R.y, cardW, R.h};
    ui::draw::line(T.right() + 12, R.y, T.right() + 12, R.bottom(), th.border, 1);
    ui::Area ca(C, 0);
    const Row* cur = nullptr;
    for (auto& r : rows)
      if (r.item->id == sel) cur = &r;
    if (cur) {
      drawCard(a, st, d, *cur);
    } else {
      ui::spacer(std::max(0.f, C.h * 0.3f));
      ui::emptyState(d.icon, "Выберите запись в таблице.");
    }
  }
  // Delete — удалить выбранную запись (вне текстовых полей).
  if (!ro && sel && ui::shortcut({Key::Delete, 0})) askRemove(a, d, sel);
}

// ---------------------------------------------------------------- выдвижная панель
void drawDrawer(App& a) {
  const World& w = a.world();
  {
    ui::HStack hs(30, ui::Align::Left, 6);
    ui::label("Списки мира", {.ink = ui::Ink::Muted});
    ui::flex();
    if (ui::iconButton("maximize", "Открыть окно справочников")) a.openEditor("catalogs", 0);
    a.markUi("drawer.catalogs.open");
  }
  ui::gap(2);
  for (int i = 0; i < kListCount; i++) {
    const ListDef& d = kLists[i];
    const auto& list = rules::catalogList(*w.catalogs, d.list);
    std::string sub;
    for (size_t k = 0; k < list.size() && k < 4; k++) sub += (k ? ", " : "") + list[k].name;
    if (list.size() > 4) sub += "…";
    if (sub.empty()) sub = "пусто";
    ui::IdScope s(i);
    const ui::Theme& th = ui::theme();
    Color tint = d.colored && !list.empty() ? list.front().color : th.textDim;
    if (d.list == CatalogList::Resources) tint = th.accent;
    if (edkit::entityRow(d.title, sub, d.icon, tint, std::to_string(list.size()), false, false, "Открыть справочник"))
      a.openEditor("catalogs", Id(i + 1));
    a.markUi(std::string("drawer.catalogs.") + std::to_string(i));
  }
}

EditorReg editorReg({"catalogs", "Справочники", drawEditor, "book"});
DrawerReg drawerReg({"catalogs", "book", "Справочники", 90, drawDrawer});
CommandReg commandReg({"editor.catalogs", "Справочники: ресурсы, расы, культуры, религии…", "book", nullptr,
                       [](App& a) { a.openEditor("catalogs", 0); }, [](App& a) { return a.ui.screen == Screen::Editor; }, false,
                       "Справочники"});

}  // namespace
}  // namespace rg::app
