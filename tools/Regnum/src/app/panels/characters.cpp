// Regnum — выдвижная панель «Персонажи»: поиск, фильтр по фракции и героям, список по фракциям (портрет или
// инициалы, имя, титул и роль, звезда героя, кольцо правителя), создание, удаление с подтверждением.
// Щелчок открывает инспектор персонажа (character_inspector.cpp).
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
std::string roleLine(const World& w, const Character& c, const Roles& r);
const gfx::Image* portraitImage(const Character& c, bool square);
void avatar(const Character& c, float size, bool ring, std::string_view tip);
void askDelete(App& a, Id character);
}  // namespace rg::app::chars

namespace rg::app {
namespace {

using platform::Key;

struct ListState {
  std::string query;
  Id faction = 0;       // фильтр по фракции (0 — все)
  bool heroes = false;  // только герои
  Id menuFor = 0;       // персонаж контекстного меню
  Id seenSel = 0;       // выделение прошлого кадра (новое — прокрутить к строке)
};

Id createAct(App& a, Id faction) {
  Id nid = 0;
  if (a.act("Новый персонаж", [&](Tx& tx) { nid = rules::createCharacter(tx, faction, ""); })) {
    a.select(SelType::Character, nid);
    a.ui.tabOf[SelType::Character] = "character.info";
  }
  return nid;
}

// Строка персонажа (44): основа — ui::listItem (наведение, выделение, фокус, меню), сверху портрет и текст.
bool characterRow(App& a, const Character& c, const chars::Roles& roles, bool selected, ui::Item& item) {
  const World& w = a.world();
  const ui::Theme& th = ui::theme();
  RectF r = ui::next(0, 44);
  ui::at(r);
  std::string tip = roles.total() ? "Роли: " + chars::rolesText(w, roles) : std::string();
  bool clicked = ui::listItem("##row", {.subtitle = " ", .selected = selected, .tooltip = tip});
  item = ui::lastItem();
  bool ruler = !roles.rulerOf.empty();
  ui::at(RectF{r.x + 8, r.cy() - 18, 36, 36});
  chars::avatar(c, 30, ruler, {});
  // Точка фракции на портрете.
  if (const Faction* f = w.faction(c.faction)) {
    float cx = r.x + 8 + 18 + 11, cy = r.cy() + 11;
    ui::draw::circle(cx, cy, 6, th.surface1);
    ui::draw::circle(cx, cy, 4.5f, f->color);
  }
  float x = r.x + 54;
  float right = r.right() - 10;
  if (c.hero) {
    RectF sr{right - 16, r.cy() - 8, 16, 16};
    ui::draw::icon("star-filled", sr, th.accent);
    right = sr.x - 8;
  }
  float lh = ui::lineHeight(ui::Font::Body), sh = ui::lineHeight(ui::Font::Small);
  float y0 = std::round(r.cy() - (lh + sh) * 0.5f);
  ui::draw::text(c.name.empty() ? std::string_view("Без имени") : std::string_view(c.name), RectF{x, y0, right - x, lh},
                 selected ? ui::Font::Strong : ui::Font::Body, th.text);
  ui::draw::text(chars::roleLine(w, c, roles), RectF{x, y0 + lh, right - x, sh}, ui::Font::Small, th.textMuted);
  return clicked;
}

void groupHeader(const std::string& title, Color dot, size_t n) {
  const ui::Theme& th = ui::theme();
  RectF r = ui::next(0, 26);
  float x = r.x + 4;
  if (dot.a > 0) {
    ui::draw::circle(x + 4, r.cy() + 1, 4, dot);
    x += 14;
  }
  std::string up = utf8::upper(title);
  float tw = std::min(ui::measure(up, ui::Font::Caption) + 2, r.w - (x - r.x) - 40);
  ui::draw::text(up, RectF{x, r.y + 2, tw, r.h}, ui::Font::Caption, th.textMuted);
  ui::draw::text(std::to_string(n), RectF{x + tw + 6, r.y + 2, 40, r.h}, ui::Font::Caption, th.textMuted.alpha(0.7f));
}

void drawDrawer(App& a) {
  const World& w = a.world();
  bool ro = a.readOnly();
  ListState& st = ui::state<ListState>(ui::id("##chars"));
  if (st.faction && !w.faction(st.faction)) st.faction = 0;
  {
    ui::Row r({ui::fr(1), ui::px(30)}, 30, 6);
    if (ui::shortcut({Key::F, ui::ModPrimary})) ui::setKeyboardFocus(ui::id("q"));
    ui::searchField("q", st.query, "Поиск персонажей");
    ui::tooltip("Поиск по имени и титулу", {Key::F, ui::ModPrimary});
    a.markUi("characters.search");
    if (ui::iconButton("user-plus", "Новый персонаж", {.variant = ui::Variant::Secondary, .disabled = ro})) createAct(a, st.faction);
    a.markUi("characters.new");
  }
  {
    ui::Row r({ui::fr(1), ui::px(30)}, 30, 6);
    w::factionPicker("faction", st.faction, w::FactionFilter::Any, "Все фракции");
    a.markUi("characters.faction");
    ui::iconToggle(st.heroes ? "star-filled" : "star", "Только значимые герои", st.heroes);
    a.markUi("characters.heroes");
  }
  // Список: по фракциям (государства, затем гильдии), без фракции — в конце.
  std::vector<const Character*> list;
  w.characters.each([&](const Character& c) {
    if (st.faction && c.faction != st.faction) return;
    if (st.heroes && !c.hero) return;
    if (!st.query.empty() && !utf8::matches(c.name, st.query) && !utf8::matches(c.title, st.query)) return;
    list.push_back(&c);
  });
  auto facRank = [&](Id f) {
    const Faction* x = w.faction(f);
    return !x ? 2 : x->isState() ? 0 : 1;
  };
  std::unordered_map<Id, chars::Roles> rmap;
  for (const Character* c : list) rmap[c->id] = chars::rolesOf(w, c->id);
  std::stable_sort(list.begin(), list.end(), [&](const Character* x, const Character* y) {
    int rx = facRank(x->faction), ry = facRank(y->faction);
    if (rx != ry) return rx < ry;
    if (x->faction != y->faction) return compareRu(w.factionName(x->faction), w.factionName(y->faction)) < 0;
    bool lx = !rmap[x->id].rulerOf.empty(), ly = !rmap[y->id].rulerOf.empty();
    if (lx != ly) return lx;   // правитель первым
    if (x->hero != y->hero) return x->hero;
    return compareRu(x->name, y->name) < 0;
  });
  if (w.characters.empty()) {
    ui::spacer(24);
    if (ui::emptyState("users", "Персонажей пока нет.", ro ? std::string_view() : "Новый персонаж", "user-plus")) createAct(a, 0);
    return;
  }
  if (list.empty()) {
    ui::spacer(16);
    ui::label("Никого не найдено", {.ink = ui::Ink::Muted, .align = ui::Align::Center});
    return;
  }
  ui::gap(2);
  bool grouped = st.faction == 0;
  bool openMenu = false;
  {
    // Поиск и фильтр закреплены сверху: список — в своей прокрутке до низа панели.
    float footH = ui::lineHeight(ui::Font::Small) + 8;
    float listH = 0;
    if (const RectF* dr = a.uiRect("drawer")) listH = dr->bottom() / ui::uiScale() - 16 - ui::avail().y - footH;
    std::optional<ui::Scroll> sc;
    if (listH > 120) sc.emplace("list", listH);
    Id curGroup = 0xFFFFFFFFu;
    for (size_t i = 0; i < list.size(); i++) {
      const Character& c = *list[i];
      if (grouped && c.faction != curGroup) {
        curGroup = c.faction;
        size_t n = 0;
        for (size_t k = i; k < list.size() && list[k]->faction == curGroup; k++) n++;
        if (i) ui::spacer(4);
        const Faction* f = w.faction(curGroup);
        groupHeader(f ? (f->name.empty() ? std::string("Без названия") : f->name) : std::string("Без фракции"), f ? f->color : Color(0, 0, 0, 0), n);
      }
      ui::IdScope s{i64(c.id)};
      ui::Item item;
      bool sel = a.ui.sel == Selection{SelType::Character, c.id};
      if (characterRow(a, c, rmap[c.id], sel, item)) a.select(SelType::Character, c.id);
      if (sel) {
        a.markUi("characters.selected");
        if (st.seenSel != c.id) ui::scrollToItem();
      }
      if (i == 0) a.markUi("characters.first");
      if (item.rightClicked) {
        st.menuFor = c.id;
        openMenu = true;
      }
    }
  }
  st.seenSel = a.ui.sel.type == SelType::Character ? a.ui.sel.id : 0;
  // Контекстное меню строки (открывается в области панели, а не строки).
  if (openMenu) ui::openContextMenu("ctx");
  if (ui::beginMenu("ctx")) {
    const Character* c = w.character(st.menuFor);
    if (c) {
      ui::menuHeader(c->name.empty() ? std::string("Без имени") : c->name);
      if (ui::menuItem("Открыть", {.icon = "user"})) a.select(SelType::Character, c->id);
      if (ui::menuItem("Показать на карте", {.icon = "target"})) {
        a.select(SelType::Character, c->id);
        a.focusSelection();
      }
      if (ui::menuItem(c->hero ? "Снять отметку героя" : "Отметить героем", {.icon = "star", .disabled = ro})) {
        Id cid = c->id;
        bool on = !c->hero;
        a.act(on ? "Отметить героем" : "Снять отметку героя", [&](Tx& tx) { tx.character(cid).hero = on; });
      }
      ui::menuSeparator();
      if (ui::menuItem("Удалить", {.icon = "trash", .danger = true, .disabled = ro})) chars::askDelete(a, c->id);
    }
    ui::endMenu();
  }
  ui::spacer(4);
  std::string cnt = fmtInt(i64(list.size())) + " " + plural(i64(list.size()), "персонаж", "персонажа", "персонажей");
  i64 heroes = std::count_if(list.begin(), list.end(), [](const Character* c) { return c->hero; });
  if (heroes) cnt += " · героев: " + fmtInt(heroes);
  ui::label(cnt, {.font = ui::Font::Small, .ink = ui::Ink::Muted, .align = ui::Align::Center});
}

DrawerReg drawerReg({"characters", "users", "Персонажи", 40, drawDrawer, "Ctrl+4"});
CommandReg cmdPanel({"panel.characters", "Персонажи", "users", nullptr,
                     [](App& a) {
                       if (a.ui.drawer != "characters") a.openDrawer("characters");
                     },
                     [](App& a) { return a.ui.screen == Screen::Editor && a.ui.editor.empty(); }, false, "Панели"});
CommandReg cmdNew({"character.new", "Новый персонаж", "user-plus", nullptr,
                   [](App& a) {
                     if (a.ui.drawer != "characters") a.openDrawer("characters");
                     createAct(a, 0);
                   },
                   [](App& a) { return a.ui.screen == Screen::Editor && a.ui.editor.empty() && !a.readOnly(); }, false, "Панели"});

}  // namespace
}  // namespace rg::app
