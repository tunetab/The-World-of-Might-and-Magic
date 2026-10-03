// Regnum — выдвижная панель «Провинции»: поиск, отбор по владельцу (все / государство / без владельца / морские),
// сортировка (название, население, торговая ценность, восстание), виртуальный список с цветом владельца,
// щелчок — выбор и показ на карте, контекстное меню (показать, удалить).
#include "app/widgets.h"

namespace rg::app::prov {
// province_common.cpp
std::string provinceTitle(const Province& p);
std::string num(double v);
std::string pct(double v, bool sign = false);
void showProvince(App& a, Id pid);
void askDelete(App& a, Id pid);
extern const char* const kDeleteNeedsEdit;
}  // namespace rg::app::prov

namespace rg::app {
namespace {

enum Sort : int { ByName, ByPopulation, ByTrade, ByRebellion };
enum Filter : int { All, NoOwner, Sea, FirstState };

struct ListState {
  const App* app = nullptr;
  u64 frame = 0;               // кадр приложения при последнем обращении
  std::string query;
  int filter = All;            // All, NoOwner, Sea или FirstState + индекс государства
  Id state = 0;                // выбранное государство (для отбора по владельцу)
  int sort = ByName;
  bool desc = false;
  u64 key = 0;                 // отпечаток входных данных кеша
  std::vector<Id> ids;         // отобранные и упорядоченные провинции
  Selection lastSel;
};
ListState& listState(App& a) {
  static ListState s;
  // Новое приложение (тесты; адрес может совпасть с прежним — счётчик кадров начинается заново) — чистое состояние.
  if (s.app != &a || a.frameCount() < s.frame) s = ListState{&a};
  s.frame = a.frameCount();
  return s;
}

u64 inputKey(App& a, const ListState& s) {
  u64 h = hash64(s.query);
  h = hashMix(h, a.store.version());
  h = hashMix(h, a.ui.viewTurn ? u64(*a.ui.viewTurn) + 1 : 0);
  h = hashMix(h, u64(s.filter) * 31 + s.state);
  h = hashMix(h, u64(s.sort) * 2 + (s.desc ? 1 : 0));
  return h;
}

void rebuild(App& a, ListState& s) {
  const World& wd = a.world();
  auto calc = rules::calc(wd);
  struct Item {
    Id id;
    const Province* p;
    double v;
  };
  std::vector<Item> items;
  items.reserve(wd.provinces.size());
  wd.provinces.each([&](const Province& p) {
    switch (s.filter) {
      case NoOwner:
        if (p.sea || p.owner) return;
        break;
      case Sea:
        if (!p.sea) return;
        break;
      case All: break;
      default:
        if (p.sea || p.owner != s.state) return;
        break;
    }
    if (!s.query.empty()) {
      std::string hay = p.name + " " + p.capital + " " + (p.sea ? std::string() : wd.factionName(p.owner));
      if (!utf8::matches(hay, s.query)) return;
    }
    double v = 0;
    const rules::ProvinceCalc* pc = calc->province(p.id);
    if (pc && !p.sea) {
      if (s.sort == ByPopulation) v = double(pc->population);
      else if (s.sort == ByTrade) v = pc->tradeValue;
      else if (s.sort == ByRebellion) v = pc->rebellion;
    }
    items.push_back(Item{p.id, &p, v});
  });
  std::stable_sort(items.begin(), items.end(), [&](const Item& x, const Item& y) {
    if (s.sort != ByName && x.v != y.v) return s.desc ? x.v > y.v : x.v < y.v;
    int c = compareRu(prov::provinceTitle(*x.p), prov::provinceTitle(*y.p));
    if (c != 0) return s.sort == ByName && s.desc ? c > 0 : c < 0;
    return x.id < y.id;
  });
  s.ids.clear();
  for (const Item& it : items) s.ids.push_back(it.id);
}

void drawList(App& a) {
  const World& wd = a.world();
  ListState& s = listState(a);
  bool ro = a.readOnly();

  // Государства для отбора (по алфавиту).
  std::vector<const Faction*> states;
  wd.factions.each([&](const Faction& f) {
    if (f.isState()) states.push_back(&f);
  });
  std::sort(states.begin(), states.end(), [](const Faction* x, const Faction* y) { return compareRu(x->name, y->name) < 0; });
  if (s.filter >= FirstState) {
    auto it = std::find_if(states.begin(), states.end(), [&](const Faction* f) { return f->id == s.state; });
    if (it == states.end()) {
      s.filter = All;
      s.state = 0;
    } else {
      s.filter = FirstState + int(it - states.begin());
    }
  }

  // Поиск, отбор, сортировка
  ui::searchField("q", s.query, "Поиск провинции");
  a.markUi("provinces.search");
  {
    ui::Row r({ui::fr(1), ui::px(30), ui::px(30)}, 30, 4);
    std::vector<std::string> labels{"Все провинции", "Без владельца", "Морские"};
    for (const Faction* f : states) labels.push_back(f->name.empty() ? std::string("Государство") : f->name);
    std::vector<ui::Option> opts;
    opts.reserve(labels.size());
    opts.push_back(ui::Option{labels[0], "province"});
    opts.push_back(ui::Option{labels[1], "flag"});
    opts.push_back(ui::Option{labels[2], "sea"});
    for (size_t i = 0; i < states.size(); i++) opts.push_back(ui::Option{labels[i + 3], nullptr, states[i]->color});
    int f = s.filter;
    if (ui::combo("filter", f, opts, {.icon = "filter", .popupWidth = 240, .tooltip = "Отбор по владельцу"})) {
      s.filter = std::max(0, f);
      s.state = s.filter >= FirstState ? states[size_t(s.filter - FirstState)]->id : 0;
    }
    a.markUi("provinces.filter");
    // Сортировка: значок текущего порядка, меню вариантов.
    static const char* sortIcons[] = {"sort", "population", "trade-value", "rebellion"};
    static const char* sortNames[] = {"По названию", "По населению", "По торговой ценности", "По риску восстания"};
    bool openSort = ui::iconButton(sortIcons[clamp(s.sort, 0, 3)], std::string("Сортировка: ") + sortNames[clamp(s.sort, 0, 3)]);
    if (openSort) ui::openPopup("sortmenu");
    a.markUi("provinces.sort");
    if (ui::beginMenu("sortmenu")) {
      ui::menuHeader("Сортировка");
      for (int i = 0; i < 4; i++) {
        ui::IdScope si{i};
        if (ui::menuItem(sortNames[i], {.icon = sortIcons[i], .checked = s.sort == i})) {
          s.sort = i;
          s.desc = i != ByName;   // числа — сначала крупные
        }
        a.markUi("provinces.sort." + std::to_string(i));
      }
      ui::endMenu();
    }
    if (ui::iconButton(s.desc ? "sort-desc" : "sort-asc", s.desc ? "По убыванию" : "По возрастанию")) s.desc = !s.desc;
    a.markUi("provinces.dir");
  }

  u64 k = inputKey(a, s);
  if (k != s.key) {
    rebuild(a, s);
    s.key = k;
  }
  int total = int(wd.provinces.size());
  int n = int(s.ids.size());
  {
    ui::HStack hs(22, ui::Align::Left, 8);
    ui::caption(n == total ? "Провинции" : "Найдено");
    ui::flex();
    ui::badge(n == total ? fmtNum(n) : fmtNum(n) + " из " + fmtNum(total), ui::Tone::Neutral);
    a.markUi("provinces.count");
  }

  if (total == 0) {
    ui::spacer(12);
    if (ui::emptyState("province", "Провинций пока нет.", ro ? std::string_view() : "Нарисовать провинцию", "tool-polygon")) {
      a.setEditBorders(true);
      a.setTool(ToolId::NewProvince);
    }
    return;
  }
  if (n == 0) {
    ui::spacer(12);
    if (ui::emptyState("search", "Ничего не найдено.", "Сбросить отбор", "close")) {
      s.query.clear();
      s.filter = All;
      s.state = 0;
    }
    return;
  }

  // Список: высота — до низа панели (панель сама прокручивается, список — виртуальный).
  const float rowH = 46;
  float top = ui::avail().y;
  float bottom = top + 520;
  if (const RectF* dr = a.uiRect("drawer")) bottom = dr->bottom() / ui::uiScale() - 14;
  float h = std::max(rowH * 3, std::floor(bottom - top - 2));
  ui::VirtualList vl("rows", n, rowH, h);
  auto calc = rules::calc(wd);
  for (int i : vl) {
    Id pid = s.ids[size_t(i)];
    const Province* p = wd.province(pid);
    if (!p) continue;
    const Faction* own = p->sea ? nullptr : wd.faction(p->owner);
    std::string name = prov::provinceTitle(*p);
    std::string sub;
    if (p->sea) sub = "Морская провинция";
    else {
      sub = own ? own->name : std::string("без владельца");
      if (!p->capital.empty()) sub += " · " + p->capital;
    }
    std::string hint;
    if (const rules::ProvinceCalc* pc = calc->province(pid); pc && !p->sea) {
      if (s.sort == ByTrade) hint = prov::num(pc->tradeValue);
      else if (s.sort == ByRebellion) hint = prov::pct(pc->rebellion);
      else hint = pc->population > 0 ? fmtShort(double(pc->population)) : std::string("—");
    }
    ui::ListItemOpt o;
    if (own) o.dot = own->color;
    else o.icon = p->sea ? "sea" : "province";
    o.subtitle = sub;
    o.hint = hint;
    o.selected = a.ui.sel == Selection{SelType::Province, pid};
    if (ui::listItem(name, o)) prov::showProvince(a, pid);
    a.markUi("provinces.row." + std::to_string(pid));
    if (ui::beginContextMenu("ctx")) {
      ui::menuHeader(name);
      if (ui::menuItem("Показать на карте", {.icon = "target"})) prov::showProvince(a, pid);
      if (own && ui::menuItem("Открыть государство", {.icon = "crown"})) a.select(SelType::Faction, own->id);
      ui::menuSeparator();
      if (ui::menuItem("Удалить провинцию", {.icon = "trash", .danger = true, .disabled = ro || !a.ui.editBorders})) prov::askDelete(a, pid);
      if (!ro && !a.ui.editBorders) ui::tooltip(prov::kDeleteNeedsEdit);
      a.markUi("provinces.ctx.delete");
      ui::endMenu();
    }
  }
  // Выбор на карте — прокрутить к строке.
  if (a.ui.sel != s.lastSel) {
    s.lastSel = a.ui.sel;
    if (a.ui.sel.type == SelType::Province) {
      auto it = std::find(s.ids.begin(), s.ids.end(), a.ui.sel.id);
      if (it != s.ids.end()) {
        int row = int(it - s.ids.begin());
        if (row < vl.first() || row >= vl.last() - 1) vl.scrollToRow(row);
      }
    }
  }
}

DrawerReg drawer({"provinces", "province", "Провинции", 10, drawList, "Ctrl+1"});

}  // namespace
}  // namespace rg::app
