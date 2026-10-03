// Regnum — окно модификаторов (ТЗ 1.g): список с поиском и фильтром, создание, копия, удаление с предупреждением
// о местах использования; карточка модификатора (название, значок, цвет, описание), все эффекты schema::kEffects
// в пределах ТЗ с единицами, цели дипломатии, живой предпросмотр и «Где используется» со ссылками.
// Быстрый доступ — выдвижная панель «Модификаторы» на ленте слева и команда палитры.
#include <algorithm>

#include "app/app_internal.h"
#include "app/widgets.h"
#include "gfx/icons.h"

namespace rg::app {

// Общие элементы редакторов справочников (определены ниже, используются и в catalogs.cpp).
namespace edkit {
bool iconPicker(std::string_view id, std::string& icon, bool allowNone, bool disabled, std::string_view tip);
const char* iconTitle(std::string_view icon);
void iconTile(const std::string& icon, Color tint, float size, const char* fallback);
bool entityRow(std::string_view title, std::string_view subtitle, const char* icon, Color tint, std::string_view hint, bool selected,
               bool warn, std::string_view tip);
void goTo(App& a, Selection s);
void chipsBegin();
ui::ChipAction chip(std::string_view label, const ui::ChipOpt& o);
void chipsEnd();
void effectChips(const Modifier& m);
}  // namespace edkit

namespace {

using platform::Key;
using detail::later;

// Фишки с переносом строк (ряд ui::HStack не переносит).
struct ChipFlow {
  ChipFlow() { edkit::chipsBegin(); }
  ~ChipFlow() { edkit::chipsEnd(); }
  ChipFlow(const ChipFlow&) = delete;
  ChipFlow& operator=(const ChipFlow&) = delete;
};

template <class V, class T>
bool has(const V& v, const T& x) {
  return std::find(v.begin(), v.end(), x) != v.end();
}
std::string orName(const std::string& s, const char* fallback) { return s.empty() ? std::string(fallback) : s; }

// ---------------------------------------------------------------- значки на выбор
struct IconChoice {
  const char* name;
  const char* title;
};
const IconChoice kIcons[] = {
    {"sparkles", "Чудо"},        {"star", "Звезда"},          {"crown", "Корона"},        {"scroll", "Указ"},
    {"book", "Знание"},          {"tech", "Наука"},           {"research", "Исследование"}, {"wand", "Магия"},
    {"bolt", "Молния"},          {"sun", "Солнце"},           {"moon", "Луна"},           {"eye", "Око"},
    {"coins", "Монеты"},         {"treasury", "Казна"},       {"income", "Доход"},        {"expense", "Расход"},
    {"trade", "Торговля"},       {"trade-value", "Торговая ценность"}, {"route", "Тракт"}, {"scales", "Весы"},
    {"handshake", "Сделка"},     {"diplomacy", "Дипломатия"}, {"alliance", "Союз"},       {"war", "Война"},
    {"population", "Население"}, {"contentment", "Довольство"}, {"discontent", "Недовольство"}, {"rebellion", "Мятеж"},
    {"heart", "Здоровье"},       {"religion", "Вера"},        {"culture", "Культура"},    {"race", "Народ"},
    {"grain", "Зерно"},          {"wood", "Древесина"},       {"stone", "Камень"},        {"iron", "Железо"},
    {"gem", "Самоцвет"},         {"pickaxe", "Добыча"},       {"resource", "Ресурс"},     {"magnet", "Притяжение"},
    {"hammer", "Ремесло"},       {"factory", "Мастерская"},   {"building", "Постройка"},  {"house", "Жильё"},
    {"castle", "Замок"},         {"tower", "Башня"},          {"slots", "Слоты"},         {"shield", "Защита"},
    {"sword", "Меч"},            {"swords", "Битва"},         {"army", "Войско"},         {"bow", "Стрелки"},
    {"horse", "Конница"},        {"fleet", "Флот"},           {"anchor", "Гавань"},       {"skull", "Гибель"},
    {"mountain", "Горы"},        {"sea", "Море"},             {"island", "Остров"},       {"globe", "Мир"},
    {"compass", "Путь"},         {"flag", "Флаг"},            {"banner", "Знамя"},        {"hourglass", "Время"},
    {"dice", "Удача"},           {"lock", "Запрет"},          {"chronicle", "Летопись"},  {"capital", "Столица"},
};

// ---------------------------------------------------------------- использование
struct ModUse {
  std::vector<Id> provinces, states, guilds, techs;
  std::vector<std::pair<Id, int>> levels;   // постройка, номер уровня (с 1)
  size_t total() const { return provinces.size() + states.size() + guilds.size() + techs.size() + levels.size(); }
};

ModUse usageOf(const World& w, Id mod) {
  ModUse u;
  w.provinces.each([&](const Province& p) {
    if (has(p.modifiers, mod)) u.provinces.push_back(p.id);
  });
  w.factions.each([&](const Faction& f) {
    if (has(f.modifiers, mod)) (f.isGuild() ? u.guilds : u.states).push_back(f.id);
  });
  w.techs.each([&](const Tech& t) {
    if (has(t.modifiers, mod)) u.techs.push_back(t.id);
  });
  w.buildings.each([&](const Building& b) {
    for (size_t i = 0; i < b.levels.size(); i++)
      if (has(b.levels[i].modifiers, mod)) u.levels.push_back({b.id, int(i) + 1});
  });
  std::sort(u.provinces.begin(), u.provinces.end(), [&](Id x, Id y) { return compareRu(w.provinceName(x), w.provinceName(y)) < 0; });
  std::sort(u.states.begin(), u.states.end(), [&](Id x, Id y) { return compareRu(w.factionName(x), w.factionName(y)) < 0; });
  std::sort(u.guilds.begin(), u.guilds.end(), [&](Id x, Id y) { return compareRu(w.factionName(x), w.factionName(y)) < 0; });
  return u;
}

// Число мест использования каждого модификатора (для списка).
std::unordered_map<Id, int> usageCounts(const World& w) {
  std::unordered_map<Id, int> n;
  w.provinces.each([&](const Province& p) {
    for (Id m : p.modifiers) n[m]++;
  });
  w.factions.each([&](const Faction& f) {
    for (Id m : f.modifiers) n[m]++;
  });
  w.techs.each([&](const Tech& t) {
    for (Id m : t.modifiers) n[m]++;
  });
  w.buildings.each([&](const Building& b) {
    for (auto& l : b.levels)
      for (Id m : l.modifiers) n[m]++;
  });
  return n;
}

std::string usageText(const ModUse& u) {
  std::vector<std::string> parts;
  auto add = [&](size_t n, const char* one, const char* few, const char* many) {
    if (n) parts.push_back(fmtInt(i64(n)) + "\xC2\xA0" + plural(i64(n), one, few, many));
  };
  add(u.provinces.size(), "провинция", "провинции", "провинций");
  add(u.states.size(), "государство", "государства", "государств");
  add(u.guilds.size(), "гильдия", "гильдии", "гильдий");
  add(u.techs.size(), "технология", "технологии", "технологий");
  add(u.levels.size(), "уровень постройки", "уровня построек", "уровней построек");
  std::string s;
  for (size_t i = 0; i < parts.size(); i++) s += (i ? ", " : "") + parts[i];
  return s;
}

int effectCount(const Modifier& m) {
  int n = 0;
  for (int f = 0; f < kFxCount; f++) n += m.has(Fx(f)) ? 1 : 0;
  return n;
}
bool hasLocal(const Modifier& m) {
  for (int f = 0; f < kFxCount; f++)
    if (m.has(Fx(f)) && schema::kEffects[f].local) return true;
  return false;
}
bool hasGlobal(const Modifier& m) {
  for (int f = 0; f < kFxCount; f++)
    if (m.has(Fx(f)) && !schema::kEffects[f].local) return true;
  return false;
}
// Дипломатия включена, а цели не выбраны (ТЗ 1.g.ii.2.b: «с указанными для модификатора государствами»).
bool missingTargets(const Modifier& m) { return m.has(Fx::DiplomacyPerTurn) && m.targets.empty(); }

// Краткая сводка эффектов для строки списка.
std::string effectSummary(const Modifier& m) {
  int n = effectCount(m);
  if (n == 0) return "Без эффектов";
  for (int f = 0; f < kFxCount; f++) {
    if (!m.has(Fx(f))) continue;
    std::string s = w::effectText(Fx(f), m.fx[size_t(f)]);
    if (n > 1) s += " · ещё " + std::to_string(n - 1);
    return s;
  }
  return {};
}

// ---------------------------------------------------------------- эффекты: единицы и шаги
const char* fxUnit(Fx f) {
  switch (f) {
    case Fx::PopGrowthPct: return "% за ход";
    case Fx::ContentmentPerTurn:
    case Fx::DiplomacyPerTurn: return "за ход";
    case Fx::ResourceFlat: return "ед.";
    case Fx::Slots: return "слот|слота|слотов";
    default: return schema::effect(f).unit[0] == '%' ? "%" : "";
  }
}
int fxDigits(Fx f) { return f == Fx::TradeFlat || f == Fx::Slots ? 0 : 1; }
double fxStep(Fx f) { return schema::effect(f).max >= 1000 ? 10 : 1; }
// Начальное значение при включении эффекта (заметное, в пределах ТЗ).
double fxDefault(Fx f) {
  double mx = schema::effect(f).max;
  if (mx <= 5) return 1;
  if (mx <= 50) return 5;
  if (mx <= 100) return 10;
  return 100;
}
std::string rangeText(Fx f) {
  const auto& e = schema::effect(f);
  std::string u = e.unit[0] == '%' ? "\xC2\xA0%" : "";
  return fmtSigned(e.min) + "…" + fmtSigned(e.max) + u;
}

// ---------------------------------------------------------------- состояние окна
struct EdState {
  std::string query;
  int filter = 0;          // 0 — все, 1 — с локальными, 2 — с глобальными, 3 — нигде не используются
  bool focusName = false;
  bool focusSearch = false;
  std::string seenQuery;   // запрос и фильтр прошлого кадра (смена — выделение к первому найденному)
  int seenFilter = 0;
  Id seenSel = 0;          // выделение прошлого кадра (новое — прокрутить список к строке)
};

bool passes(const Modifier& m, const EdState& st, const std::unordered_map<Id, int>& uses) {
  if (!st.query.empty() && !utf8::matches(m.name, st.query) && !utf8::matches(m.desc, st.query)) return false;
  switch (st.filter) {
    case 1: return hasLocal(m);
    case 2: return hasGlobal(m);
    case 3: {
      auto it = uses.find(m.id);
      return it == uses.end() || it->second == 0;
    }
    default: return true;
  }
}

std::vector<const Modifier*> sortedModifiers(const World& w) {
  std::vector<const Modifier*> list;
  w.modifiers.each([&](const Modifier& m) { list.push_back(&m); });
  std::sort(list.begin(), list.end(), [](const Modifier* a, const Modifier* b) {
    int c = compareRu(a->name, b->name);
    return c != 0 ? c < 0 : a->id < b->id;
  });
  return list;
}

// ---------------------------------------------------------------- действия
Id createModifierAct(App& a) {
  Id nid = 0;
  a.act("Новый модификатор", [&](Tx& tx) { nid = rules::createModifier(tx); });
  return nid;
}

Id duplicateAct(App& a, Id src) {
  const Modifier* m = a.world().modifier(src);
  if (!m) return 0;
  Modifier copy = *m;
  Id nid = 0;
  a.act("Копия модификатора", [&](Tx& tx) {
    nid = rules::createModifier(tx, orName(copy.name, "Модификатор") + " — копия");
    Modifier& d = tx.modifier(nid);
    std::string name = d.name;
    d = copy;
    d.id = nid;
    d.name = name;
  });
  return nid;
}

// Удаление с подтверждением: в тексте — где модификатор используется. next — что выделить после.
void askDelete(App& a, Id mod, Id next) {
  const World& w = a.world();
  const Modifier* m = w.modifier(mod);
  if (!m) return;
  ModUse u = usageOf(w, mod);
  std::string name = "«" + orName(m->name, "Модификатор") + "»";
  std::string text = u.total() ? name + " используется: " + usageText(u) + ". Модификатор будет убран из всех этих списков."
                               : name + " нигде не используется.";
  text += " Действие можно отменить Ctrl+Z.";
  a.confirm("Удалить модификатор?", text, "Удалить", true, [mod, next](App& x) {
    if (x.act("Удалить модификатор", [&](Tx& tx) { rules::removeModifier(tx, mod); }) && x.ui.editor == "modifiers") x.ui.editorArg = next;
  });
}

// ---------------------------------------------------------------- список
void drawList(App& a, EdState& st, Id& sel, const std::vector<const Modifier*>& shown, const std::unordered_map<Id, int>& uses,
              size_t totalCount) {
  const World& w = a.world();
  bool ro = a.readOnly();
  {
    ui::Row r({ui::fr(1), ui::px(30)}, 30, 6);
    if (st.focusSearch) {
      ui::setKeyboardFocus(ui::id("q"));
      st.focusSearch = false;
    }
    ui::searchField("q", st.query, "Поиск модификаторов");
    a.markUi("modifiers.search");
    if (ui::iconButton("plus", "Новый модификатор", {.variant = ui::Variant::Secondary, .disabled = ro, .shortcut = {Key::Insert, 0}})) {
      if (Id nid = createModifierAct(a)) {
        sel = nid;
        st.query.clear();
        st.filter = 0;
        st.focusName = true;
      }
    }
    a.markUi("modifiers.new");
  }
  ui::segmented("filter", st.filter,
                {{"list", {}, "Все модификаторы"}, {"province", {}, "С локальными эффектами"}, {"crown", {}, "С глобальными эффектами"},
                 {"unlink", {}, "Нигде не используются"}},
                {.size = ui::Size::Small});
  a.markUi("modifiers.filter");
  RectF rest = ui::avail();
  float footH = ui::lineHeight(ui::Font::Small);
  {
    ui::Scroll sc("list", std::max(60.f, rest.h - footH - 8));
    ui::gap(2);
    if (totalCount == 0) {
      ui::spacer(24);
      if (ui::emptyState("sparkles", "Модификаторов пока нет.", ro ? std::string_view() : "Новый модификатор", "plus")) {
        if (Id nid = createModifierAct(a)) {
          sel = nid;
          st.focusName = true;
        }
      }
    } else if (shown.empty()) {
      ui::spacer(16);
      ui::label("Ничего не найдено", {.ink = ui::Ink::Muted, .align = ui::Align::Center});
    }
    for (const Modifier* m : shown) {
      ui::IdScope s{i64(m->id)};
      auto it = uses.find(m->id);
      int n = it == uses.end() ? 0 : it->second;
      std::string tip = missingTargets(*m) ? std::string("Дипломатия без государств-целей")
                        : n                ? "Используется: " + usageText(usageOf(w, m->id))
                                           : std::string("Нигде не используется");
      if (edkit::entityRow(orName(m->name, "Без названия"), effectSummary(*m), m->icon.empty() ? "sparkles" : m->icon.c_str(), m->color,
                           n ? std::to_string(n) : std::string(), m->id == sel, missingTargets(*m), tip))
        sel = m->id;
      if (m->id == sel) {
        a.markUi("modifiers.selected");
        if (st.seenSel != sel) ui::scrollToItem();
      }
    }
  }
  st.seenSel = sel;
  std::string cnt = fmtInt(i64(totalCount)) + " " + plural(i64(totalCount), "модификатор", "модификатора", "модификаторов");
  if (shown.size() != totalCount) cnt = "Показано " + fmtInt(i64(shown.size())) + " из " + fmtInt(i64(totalCount));
  ui::label(cnt, {.font = ui::Font::Small, .ink = ui::Ink::Muted});
}

// ---------------------------------------------------------------- карточка: эффект
void effectRow(App& a, const Modifier& m, Fx f, bool ro) {
  const auto& e = schema::effect(f);
  const ui::Theme& th = ui::theme();
  ui::IdScope s{int(f)};
  bool on = m.has(f);
  double v = on ? m.fx[size_t(f)] : 0;
  Id mid = m.id;
  RectF row = ui::next(0, 46);
  if (on) ui::draw::rect(row, th.accent.alpha(th.dark ? 0.06f : 0.07f), 8);
  ui::Area ar(row.inset(8, 0), 0);
  ui::Row r({ui::px(22), ui::px(30), ui::fr(1.2f, 150), ui::fr(1, 90), ui::px(136)}, 46, 10);
  bool on2 = on;
  if (ui::checkbox("##on", on2, ro)) {
    a.act(on2 ? "Включить эффект модификатора" : "Выключить эффект модификатора", [&](Tx& tx) {
      Modifier& x = tx.modifier(mid);
      if (on2) {
        x.fxMask |= 1u << int(f);
        double cur = x.fx[size_t(f)];
        x.fx[size_t(f)] = cur == 0 || cur < e.min || cur > e.max ? fxDefault(f) : cur;
      } else {
        x.fxMask &= ~(1u << int(f));
        x.fx[size_t(f)] = 0;
      }
    });
  }
  ui::tooltip(on ? "Выключить эффект" : "Включить эффект");
  a.markUi(std::string("modifiers.fx.") + e.id + ".on");
  {
    RectF ic = ui::next(30, 30);
    Color c = !on ? th.textMuted : v == 0 ? th.accent : w::effectGood(f, v) ? th.success : th.danger;
    ui::draw::rect(ic, c.alpha(on ? 0.16f : 0.08f), 8);
    ui::draw::icon(e.icon, ic.inset(6), c);
  }
  {
    ui::Group g(0, 0);
    ui::label(e.name, {.font = ui::Font::Body, .ink = on ? ui::Ink::Normal : ui::Ink::Dim});
    std::string sub = rangeText(f);
    if (e.perTurn) sub += " · каждый ход";
    if (e.extra) sub += " · слоты ТЗ 1.f.i";
    ui::label(sub, {.font = ui::Font::Caption, .ink = ui::Ink::Muted});
  }
  bool dis = ro || !on;
  double sv = v;
  ui::Tone tone = sv == 0 ? ui::Tone::Accent : (w::effectGood(f, sv) ? ui::Tone::Success : ui::Tone::Danger);
  std::string key = "mod:" + std::to_string(mid) + ":" + e.id;
  if (on) {
    if (ui::slider("s", sv, e.min, e.max, {.step = fxStep(f), .digits = fxDigits(f), .showValue = false, .disabled = ro, .tone = tone})) {
      double nv = clamp(sv, e.min, e.max);
      a.act("Эффект модификатора", [&](Tx& tx) { tx.modifier(mid).fx[size_t(f)] = nv; }, {.coalesce = key});
    }
  } else {
    // Выключенный эффект: только шкала с отметкой нуля (без ползунка).
    RectF tr = ui::next(0, 30).inset(8, 0);
    float zx = tr.x + float((0 - e.min) / (e.max - e.min)) * tr.w;
    ui::draw::rect(RectF{tr.x, tr.cy() - 2, tr.w, 4}, th.track, 2);
    ui::draw::rect(RectF{std::round(zx) - 1, tr.cy() - 6, 2, 12}, th.borderStrong, 1);
  }
  a.markUi(std::string("modifiers.fx.") + e.id + ".slider");
  double nv = v;
  if (ui::numberField("v", nv, {.min = e.min, .max = e.max, .step = fxStep(f), .digits = fxDigits(f), .unit = fxUnit(f), .sign = true,
                                .disabled = dis, .tooltip = "Значение в пределах ТЗ: " + rangeText(f)})) {
    nv = clamp(nv, e.min, e.max);
    a.act("Эффект модификатора", [&](Tx& tx) { tx.modifier(mid).fx[size_t(f)] = nv; }, {.coalesce = key});
  }
  a.markUi(std::string("modifiers.fx.") + e.id + ".value");
}

// Цели дипломатии: несколько фракций (обязательны при включённом эффекте).
void targetsRow(App& a, const Modifier& m, bool ro) {
  ui::IdScope scope("targets");
  const World& w = a.world();
  std::vector<const Faction*> fs;
  w.factions.each([&](const Faction& f) { fs.push_back(&f); });
  std::sort(fs.begin(), fs.end(), [](const Faction* x, const Faction* y) {
    if (x->kind != y->kind) return x->kind < y->kind;
    return compareRu(x->name, y->name) < 0;
  });
  Id mid = m.id;
  ui::Indent ind(70);
  {
    ui::HStack hs(22, ui::Align::Left, 6);
    ui::icon("target", missingTargets(m) ? ui::Ink::Warning : ui::Ink::Muted, 14);
    ui::label("Государства-цели", {.font = ui::Font::Small, .ink = ui::Ink::Dim});
    if (missingTargets(m)) ui::tag("Укажите хотя бы одно", ui::Tone::Warning);
  }
  // Выбранные цели — фишки (крестик убирает), ниже — выбор следующей цели с поиском.
  if (!m.targets.empty()) {
    edkit::chipsBegin();
    for (Id t : m.targets) {
      const Faction* f = w.faction(t);
      if (!f) continue;
      ui::IdScope s{i64(t)};
      ui::ChipOpt co;
      co.color = f->color;
      co.removable = !ro;
      co.clickable = true;
      co.tooltip = f->isGuild() ? "Гильдия — открыть" : "Государство — открыть";
      auto act = edkit::chip(orName(f->name, "Без названия"), co);
      if (act == ui::ChipAction::Click) edkit::goTo(a, {SelType::Faction, t});
      if (act == ui::ChipAction::Remove)
        a.act("Цели модификатора дипломатии", [&](Tx& tx) {
          auto& v = tx.modifier(mid).targets;
          v.erase(std::remove(v.begin(), v.end(), t), v.end());
        });
    }
    edkit::chipsEnd();
  }
  if (!ro) {
    std::vector<const Faction*> left;
    for (const Faction* f : fs)
      if (!has(m.targets, f->id)) left.push_back(f);
    int idx = -1;
    if (ui::combo("addtarget", idx, int(left.size()),
                  [&](int i) {
                    const Faction* f = left[size_t(i)];
                    return ui::Option{f->name, nullptr, f->color, f->isGuild() ? "гильдия" : ""};
                  },
                  {.placeholder = "Добавить государство-цель", .search = 1, .icon = "plus", .disabled = left.empty()}) &&
        idx >= 0 && idx < int(left.size())) {
      Id t = left[size_t(idx)]->id;
      a.act("Цели модификатора дипломатии", [&](Tx& tx) { tx.modifier(mid).targets.push_back(t); });
    }
    a.markUi("modifiers.targets");
  }
}

void effectGroup(App& a, const Modifier& m, bool local, bool ro) {
  int on = 0, total = 0;
  for (int f = 0; f < kFxCount; f++) {
    if (schema::kEffects[f].local != local) continue;
    total++;
    on += m.has(Fx(f)) ? 1 : 0;
  }
  std::string badge = std::to_string(on) + " из " + std::to_string(total);
  ui::Section sec(local ? "Локальные (провинция)" : "Глобальные (государство)", local ? "province" : "crown", {.badge = badge});
  a.markUi(local ? "modifiers.local" : "modifiers.global");
  if (!sec) return;
  ui::label(local ? "Действуют в провинции; у государства — во всех его провинциях; у гильдии — в провинциях её штабов."
                  : "Действуют на государство или гильдию целиком.",
            {.font = ui::Font::Small, .ink = ui::Ink::Muted, .wrap = true});
  ui::gap(2);
  for (int f = 0; f < kFxCount; f++) {
    if (schema::kEffects[f].local != local) continue;
    effectRow(a, m, Fx(f), ro);
    if (Fx(f) == Fx::DiplomacyPerTurn && m.has(Fx::DiplomacyPerTurn)) targetsRow(a, m, ro);
  }
}

// ---------------------------------------------------------------- карточка: «Где используется»
ui::ChipAction useChip(std::string_view label, const char* icon, Color dot, std::string_view tip, bool removable) {
  ui::ChipOpt co;
  co.icon = icon;
  co.color = dot;
  co.clickable = true;
  co.removable = removable;
  co.tooltip = tip;
  return edkit::chip(label, co);
}

const EditorDef* editorLike(std::string_view part) {
  for (auto& e : editors())
    if (std::string_view(e.id).find(part) != std::string_view::npos) return &e;
  return nullptr;
}


void usageSection(App& a, const Modifier& m, bool ro) {
  ui::IdScope scope("usage");
  const World& w = a.world();
  ModUse u = usageOf(w, m.id);
  Id mid = m.id;
  std::string badge = std::to_string(u.total());
  ui::Section sec("Где используется", "link", {.badge = badge});
  a.markUi("modifiers.usage");
  if (!sec) return;
  if (u.total() == 0)
    ui::label("Пока нигде — добавьте модификатор провинции или государству.", {.ink = ui::Ink::Muted, .wrap = true});
  auto group = [&](const char* title, size_t n) {
    if (!n) return false;
    ui::caption(std::string(title) + " · " + std::to_string(n));
    return true;
  };
  if (group("Провинции", u.provinces.size())) {
    if (!hasLocal(m) && hasGlobal(m)) ui::tag("Глобальные эффекты в провинции не действуют", ui::Tone::Warning, "warning");
    ChipFlow cf;
    for (Id pid : u.provinces) {
      const Province* p = w.province(pid);
      ui::IdScope s{i64(pid)};
      auto act = useChip(orName(p ? p->name : std::string(), "Без названия"), "province", w::factionColor(w, p ? p->owner : 0),
                         "Открыть провинцию", !ro);
      if (act == ui::ChipAction::Click) edkit::goTo(a, {SelType::Province, pid});
      if (act == ui::ChipAction::Remove)
        a.act("Убрать модификатор провинции", [&](Tx& tx) {
          auto& v = tx.province(pid).modifiers;
          v.erase(std::remove(v.begin(), v.end(), mid), v.end());
        });
    }
  }
  auto factionsGroup = [&](const char* title, const std::vector<Id>& ids, bool guilds) {
    if (!group(title, ids.size())) return;
    ChipFlow cf;
    for (Id fid : ids) {
      const Faction* f = w.faction(fid);
      ui::IdScope s{i64(fid)};
      auto act = useChip(orName(f ? f->name : std::string(), "Без названия"), guilds ? "guild" : "crown", w::factionColor(w, fid),
                         guilds ? "Открыть гильдию" : "Открыть государство", !ro);
      if (act == ui::ChipAction::Click) edkit::goTo(a, {SelType::Faction, fid});
      if (act == ui::ChipAction::Remove)
        a.act(guilds ? "Убрать модификатор гильдии" : "Убрать модификатор государства", [&](Tx& tx) {
          auto& v = tx.faction(fid).modifiers;
          v.erase(std::remove(v.begin(), v.end(), mid), v.end());
        });
    }
  };
  factionsGroup("Государства", u.states, false);
  factionsGroup("Торговые гильдии", u.guilds, true);
  if (group("Технологии", u.techs.size())) {
    ChipFlow cf;
    for (Id tid : u.techs) {
      const Tech* t = w.tech(tid);
      if (!t) continue;
      ui::IdScope s{i64(tid)};
      std::string label = orName(t->name, "Без названия") + " · " + w.factionName(t->faction);
      auto act = useChip(label, "tech", w::factionColor(w, t->faction), "Открыть дерево технологий", !ro);
      if (act == ui::ChipAction::Click) {
        Id fac = t->faction;
        if (const EditorDef* ed = editorLike("tech")) {
          std::string id = ed->id;
          later(a, [id, fac](App& x) { x.openEditor(id, fac); });
        } else {
          edkit::goTo(a, {SelType::Faction, fac});
        }
      }
      if (act == ui::ChipAction::Remove)
        a.act("Убрать модификатор технологии", [&](Tx& tx) {
          auto& v = tx.tech(tid).modifiers;
          v.erase(std::remove(v.begin(), v.end(), mid), v.end());
        });
    }
  }
  if (group("Уровни построек", u.levels.size())) {
    ChipFlow cf;
    for (auto [bid, lvl] : u.levels) {
      const Building* b = w.building(bid);
      if (!b) continue;
      ui::IdScope s{i64(bid) * 64 + lvl};
      std::string label = orName(b->name, "Без названия") + " · ур.\xC2\xA0" + std::to_string(lvl);
      auto act = useChip(label, b->icon.empty() || !gfx::hasIcon(b->icon) ? "building" : b->icon.c_str(),
                         b->owner ? w::factionColor(w, b->owner) : Color(0, 0, 0, 0),
                         b->owner ? "Уникальная постройка — открыть дерево построек" : "Общее дерево построек — открыть", !ro);
      if (act == ui::ChipAction::Click) {
        Id owner = b->owner;
        if (const EditorDef* ed = editorLike("build")) {
          std::string id = ed->id;
          later(a, [id, owner](App& x) { x.openEditor(id, owner); });
        } else if (owner) {
          edkit::goTo(a, {SelType::Faction, owner});
        }
      }
      if (act == ui::ChipAction::Remove) {
        int li = lvl - 1;
        Id bb = bid;
        a.act("Убрать модификатор уровня постройки", [&](Tx& tx) {
          Building& x = tx.building(bb);
          if (li < int(x.levels.size())) {
            auto& v = x.levels[size_t(li)].modifiers;
            v.erase(std::remove(v.begin(), v.end(), mid), v.end());
          }
        });
      }
    }
  }
  if (ro) return;
  // Добавить в списки провинции и фракции (ТЗ 1.g.i: модификатор доступен в списках провинций и государств).
  ui::spacer(4);
  ui::Row r({ui::fr(1), ui::fr(1)}, 30, 8);
  {
    std::vector<const Province*> ps;
    w.provinces.each([&](const Province& p) {
      if (!p.sea && !has(p.modifiers, mid)) ps.push_back(&p);
    });
    std::sort(ps.begin(), ps.end(), [](const Province* x, const Province* y) { return compareRu(x->name, y->name) < 0; });
    int idx = -1;
    if (ui::combo("addprov", idx, int(ps.size()),
                  [&](int i) {
                    const Province* p = ps[size_t(i)];
                    return ui::Option{p->name, "province", w::factionColor(w, p->owner), p->owner ? std::string_view() : "без владельца"};
                  },
                  {.placeholder = "Добавить провинции", .search = 1, .icon = "plus", .disabled = ps.empty(),
                   .tooltip = "Добавить модификатор в список провинции"}) &&
        idx >= 0 && idx < int(ps.size())) {
      Id pid = ps[size_t(idx)]->id;
      a.act("Модификатор провинции", [&](Tx& tx) { tx.province(pid).modifiers.push_back(mid); });
    }
    a.markUi("modifiers.addProvince");
  }
  {
    std::vector<const Faction*> fs;
    w.factions.each([&](const Faction& f) {
      if (!has(f.modifiers, mid)) fs.push_back(&f);
    });
    std::sort(fs.begin(), fs.end(), [](const Faction* x, const Faction* y) {
      if (x->kind != y->kind) return x->kind < y->kind;
      return compareRu(x->name, y->name) < 0;
    });
    int idx = -1;
    if (ui::combo("addfac", idx, int(fs.size()),
                  [&](int i) {
                    const Faction* f = fs[size_t(i)];
                    return ui::Option{f->name, nullptr, f->color, f->isGuild() ? "гильдия" : ""};
                  },
                  {.placeholder = "Добавить государству", .search = 1, .icon = "plus", .disabled = fs.empty(),
                   .tooltip = "Добавить модификатор в список государства (ТЗ 1.b.iii) или гильдии"}) &&
        idx >= 0 && idx < int(fs.size())) {
      Id fid = fs[size_t(idx)]->id;
      a.act(fs[size_t(idx)]->isGuild() ? "Модификатор гильдии" : "Модификатор государства", [&](Tx& tx) { tx.faction(fid).modifiers.push_back(mid); });
    }
    a.markUi("modifiers.addFaction");
  }
}

// ---------------------------------------------------------------- карточка модификатора
void drawDetail(App& a, EdState& st, const Modifier& m, Id next) {
  const World& w = a.world();
  bool ro = a.readOnly();
  Id mid = m.id;
  ui::Scroll sc("detail");
  // Шапка: плитка значка, название, действия.
  {
    ui::Row head({ui::px(60), ui::fr(1), ui::px(30), ui::px(30)}, 60, 12);
    edkit::iconTile(m.icon, m.color, 60, "sparkles");
    {
      ui::Group g(0, 4);
      ui::caption(hasGlobal(m) && hasLocal(m) ? "Модификатор · локальный и глобальный"
                  : hasGlobal(m)              ? "Модификатор · глобальный"
                  : hasLocal(m)               ? "Модификатор · локальный"
                                              : "Модификатор");
      std::string name = m.name;
      if (st.focusName) {
        ui::setKeyboardFocus(ui::id("name"));
        st.focusName = false;
      }
      if (ui::textField("name", name, {.placeholder = "Название модификатора", .icon = "edit", .maxLength = 80, .readOnly = ro,
                                       .selectAllOnFocus = true}))
        a.act("Переименовать модификатор", [&](Tx& tx) { tx.modifier(mid).name = trim(name); });
      a.markUi("modifiers.name");
    }
    {
      ui::Group g(30, 0);
      ui::spacer(22);
      if (ui::iconButton("duplicate", "Копия модификатора", {.disabled = ro, .shortcut = {Key::D, ui::ModPrimary}})) {
        if (Id nid = duplicateAct(a, mid)) a.ui.editorArg = nid;
      }
      a.markUi("modifiers.duplicate");
    }
    {
      ui::Group g(30, 0);
      ui::spacer(22);
      if (ui::iconButton("trash", "Удалить модификатор", {.disabled = ro, .shortcut = {Key::Delete, 0}, .tone = ui::Tone::Danger}))
        askDelete(a, mid, next);
      a.markUi("modifiers.delete");
    }
  }
  ui::spacer(4);
  // Оформление и предпросмотр.
  {
    ui::Row r({ui::fr(1), ui::fr(1)}, ui::kAuto, 12);
    {
      ui::Group g;
      ui::Card c({.icon = "palette", .title = "Оформление"});
      {
        ui::Row pr({ui::px(76), ui::fr(1)}, 30, 8);
        ui::label("Значок", {.ink = ui::Ink::Dim});
        {
          ui::HStack hs(30, ui::Align::Left, 8);
          std::string icon = m.icon;
          if (edkit::iconPicker("icon", icon, false, ro, "Значок модификатора") && icon != m.icon)
            a.act("Значок модификатора", [&](Tx& tx) { tx.modifier(mid).icon = icon; });
          a.markUi("modifiers.icon");
          ui::label(edkit::iconTitle(m.icon), {.ink = ui::Ink::Dim});
        }
        ui::label("Цвет", {.ink = ui::Ink::Dim});
        Color col = m.color;
        ui::Disabled dcol(ro);
        if (ui::colorButton("color", col, {.tooltip = "Цвет модификатора"}) && !ro)
          a.act("Цвет модификатора", [&](Tx& tx) { tx.modifier(mid).color = col; }, {.coalesce = "modcolor:" + std::to_string(mid)});
        a.markUi("modifiers.color");
      }
      std::string desc = m.desc;
      if (ui::textArea("desc", desc, 64, {.placeholder = "Описание: откуда берётся и что даёт", .readOnly = ro}))
        a.act("Описание модификатора", [&](Tx& tx) { tx.modifier(mid).desc = desc; });
      a.markUi("modifiers.desc");
    }
    {
      ui::Group g;
      ui::Card c({.icon = "eye", .title = "Предпросмотр"});
      {
        ui::HStack hs(26, ui::Align::Left, 6);
        ui::ChipOpt co;
        co.icon = m.icon.empty() ? "sparkles" : m.icon.c_str();
        co.color = m.color;
        co.tooltip = m.desc;
        ui::chip(orName(m.name, "Модификатор"), co);
        if (missingTargets(m)) ui::tag("Нет целей дипломатии", ui::Tone::Warning, "warning");
      }
      edkit::effectChips(m);
      a.markUi("modifiers.preview");
      ModUse u = usageOf(w, mid);
      ui::Row tiles({ui::fr(1), ui::fr(1)}, 60, 8);
      ui::stat(fmtInt(i64(u.provinces.size())), "Провинции", {.icon = "province", .tone = ui::Tone::Info});
      ui::stat(fmtInt(i64(u.states.size() + u.guilds.size())), "Государства и гильдии", {.icon = "crown", .tone = ui::Tone::Accent});
      ui::stat(fmtInt(i64(u.techs.size())), "Технологии", {.icon = "tech", .tone = ui::Tone::Success});
      ui::stat(fmtInt(i64(u.levels.size())), "Уровни построек", {.icon = "building", .tone = ui::Tone::Warning});
    }
  }
  // Эффекты (ТЗ 1.g.ii): на широком экране — рядом, иначе друг под другом.
  if (ui::avail().w >= 1180) {
    ui::Row r({ui::fr(1), ui::fr(1)}, ui::kAuto, 12);
    {
      ui::Group g;
      effectGroup(a, m, true, ro);
    }
    {
      ui::Group g;
      effectGroup(a, m, false, ro);
    }
  } else {
    effectGroup(a, m, true, ro);
    effectGroup(a, m, false, ro);
  }
  usageSection(a, m, ro);
  ui::spacer(8);
}

// ---------------------------------------------------------------- окно
void drawEditor(App& a, Id arg) {
  const World& w = a.world();
  const ui::Theme& th = ui::theme();
  EdState& st = ui::state<EdState>(ui::id("##modstate"));
  auto all = sortedModifiers(w);
  auto uses = usageCounts(w);
  std::vector<const Modifier*> shown;
  for (const Modifier* m : all)
    if (passes(*m, st, uses)) shown.push_back(m);
  // Выделение — аргумент окна; нет такого — первый из видимых.
  Id sel = arg;
  bool refiltered = st.query != st.seenQuery || st.filter != st.seenFilter;
  st.seenQuery = st.query;
  st.seenFilter = st.filter;
  bool visible = std::any_of(shown.begin(), shown.end(), [&](const Modifier* m) { return m->id == sel; });
  if (!w.modifier(sel) || (refiltered && !visible && !shown.empty()))
    sel = shown.empty() ? (all.empty() ? 0 : all.front()->id) : shown.front()->id;
  auto neighbor = [&](Id id) -> Id {
    for (size_t i = 0; i < shown.size(); i++)
      if (shown[i]->id == id) {
        if (i + 1 < shown.size()) return shown[i + 1]->id;
        if (i > 0) return shown[i - 1]->id;
      }
    return 0;
  };
  if (ui::shortcut({Key::F, ui::ModPrimary})) st.focusSearch = true;

  RectF R = ui::avail();
  float listW = std::round(clamp(R.w * 0.27f, 280.f, 380.f));
  RectF L{R.x, R.y, listW, R.h};
  RectF D{R.x + listW + 24, R.y, R.w - listW - 24, R.h};
  ui::draw::line(L.right() + 12, R.y, L.right() + 12, R.bottom(), th.border, 1);
  {
    ui::Area la(L, 0);
    drawList(a, st, sel, shown, uses, all.size());
  }
  {
    ui::Area da(D, 0);
    const Modifier* m = a.world().modifier(sel);
    if (!m) {
      ui::spacer(std::max(0.f, D.h * 0.3f));
      ui::emptyState("sparkles", "Выберите модификатор слева или создайте новый.");
    } else {
      drawDetail(a, st, *m, neighbor(sel));
    }
  }
  // Стрелки — по списку (если их не забрало поле или ползунок).
  if (!shown.empty()) {
    int idx = -1;
    for (size_t i = 0; i < shown.size(); i++)
      if (shown[i]->id == sel) idx = int(i);
    if (ui::shortcut({Key::Down, 0})) sel = shown[size_t(std::min(int(shown.size()) - 1, idx + 1))]->id;
    if (ui::shortcut({Key::Up, 0})) sel = shown[size_t(std::max(0, idx - 1))]->id;
  }
  // Выбор в списке — в аргумент окна (если действие кадра не выбрало другое: копия, удаление).
  if (a.ui.editor == "modifiers" && a.ui.editorArg == arg) a.ui.editorArg = sel;
}

// ---------------------------------------------------------------- выдвижная панель
void drawDrawer(App& a) {
  const World& w = a.world();
  bool ro = a.readOnly();
  auto& q = ui::state<std::string>(ui::id("##q"));
  {
    ui::Row r({ui::fr(1), ui::px(30), ui::px(30)}, 30, 6);
    if (ui::shortcut({Key::F, ui::ModPrimary})) ui::setKeyboardFocus(ui::id("q"));
    ui::searchField("q", q, "Поиск");
    if (ui::iconButton("plus", "Новый модификатор", {.disabled = ro})) {
      if (Id nid = createModifierAct(a)) a.openEditor("modifiers", nid);
    }
    a.markUi("drawer.modifiers.new");
    if (ui::iconButton("maximize", "Открыть окно модификаторов")) a.openEditor("modifiers", 0);
    a.markUi("drawer.modifiers.open");
  }
  auto all = sortedModifiers(w);
  auto uses = usageCounts(w);
  if (all.empty()) {
    ui::spacer(24);
    if (ui::emptyState("sparkles", "Модификаторов пока нет.", ro ? std::string_view() : "Новый модификатор", "plus"))
      if (Id nid = createModifierAct(a)) a.openEditor("modifiers", nid);
    return;
  }
  ui::gap(2);
  int shownN = 0;
  float listH = 0;
  if (const RectF* dr = a.uiRect("drawer")) listH = dr->bottom() / ui::uiScale() - 16 - ui::avail().y;
  std::optional<ui::Scroll> sc;   // поиск закреплён сверху, список прокручивается
  if (listH > 120) sc.emplace("list", listH);
  for (const Modifier* m : all) {
    if (!q.empty() && !utf8::matches(m->name, q)) continue;
    shownN++;
    ui::IdScope s{i64(m->id)};
    auto it = uses.find(m->id);
    int n = it == uses.end() ? 0 : it->second;
    bool open = edkit::entityRow(orName(m->name, "Без названия"), effectSummary(*m), m->icon.empty() ? "sparkles" : m->icon.c_str(), m->color,
                                 n ? std::to_string(n) : std::string(), false, missingTargets(*m), {});
    if (open) a.openEditor("modifiers", m->id);
    if (ui::beginTooltip(300)) {
      ui::label(orName(m->name, "Модификатор"), {.font = ui::Font::Strong});
      if (!m->desc.empty()) ui::text(m->desc, ui::Font::Small, ui::Ink::Dim);
      edkit::effectChips(*m);
      ui::label(n ? "Используется: " + usageText(usageOf(w, m->id)) : std::string("Нигде не используется"),
                {.font = ui::Font::Small, .ink = ui::Ink::Muted, .wrap = true});
      ui::endTooltip();
    }
  }
  if (shownN == 0) ui::label("Ничего не найдено", {.ink = ui::Ink::Muted, .align = ui::Align::Center});
}

EditorReg editorReg({"modifiers", "Модификаторы", drawEditor, "sparkles"});
DrawerReg drawerReg({"modifiers", "sparkles", "Модификаторы", 80, drawDrawer, "Ctrl+8"});
CommandReg commandReg({"editor.modifiers", "Окно модификаторов", "sparkles", nullptr, [](App& a) { a.openEditor("modifiers", 0); },
                       [](App& a) { return a.ui.screen == Screen::Editor; }, false, "Справочники"});

}  // namespace

// ================================================================ общие элементы редакторов
namespace edkit {

const char* iconTitle(std::string_view icon) {
  for (auto& c : kIcons)
    if (icon == c.name) return c.title;
  return icon.empty() ? "Без значка" : "Свой значок";
}

// Кнопка-значок с выбором из сетки во всплывающем окне. true — выбран другой значок.
bool iconPicker(std::string_view id, std::string& icon, bool allowNone, bool disabled, std::string_view tip) {
  ui::IdScope scope(id);
  bool none = icon.empty() || !gfx::hasIcon(icon);
  std::string t = std::string(tip) + ": " + iconTitle(none ? std::string_view() : std::string_view(icon));
  if (ui::iconButton(none ? "image" : icon.c_str(), t, {.variant = ui::Variant::Secondary, .disabled = disabled})) ui::openPopup("grid");
  bool changed = false;
  if (ui::beginPopup("grid", {.side = ui::Side::Below, .width = 8 * 34 + 7 * 4 + 16})) {
    ui::caption(tip);
    {
      ui::Row g({ui::px(34), ui::px(34), ui::px(34), ui::px(34), ui::px(34), ui::px(34), ui::px(34), ui::px(34)}, 34, 4);
      if (allowNone && ui::iconButton("close", "Без значка", {.toggled = none})) {
        changed = !icon.empty();
        icon.clear();
        ui::closePopup();
      }
      for (auto& c : kIcons) {
        if (!gfx::hasIcon(c.name)) continue;
        if (ui::iconButton(c.name, c.title, {.toggled = icon == c.name})) {
          changed = icon != c.name;
          icon = c.name;
          ui::closePopup();
        }
      }
    }
    ui::endPopup();
  }
  return changed;
}

// Плитка значка цвета сущности (крупный значок в шапке карточки).
void iconTile(const std::string& icon, Color tint, float size, const char* fallback) {
  const ui::Theme& th = ui::theme();
  RectF r = ui::next(size, size);
  float rad = size >= 48 ? 14 : 8;
  ui::draw::rect(r, tint.alpha(th.dark ? 0.2f : 0.16f), rad);
  ui::draw::rectStroke(r, tint.alpha(0.5f), rad, 1);
  bool ok = !icon.empty() && gfx::hasIcon(icon);
  ui::draw::icon(ok ? std::string_view(icon) : std::string_view(fallback), r.inset(size * 0.24f), tint);
}

// Строка списка сущности (высота 44): плитка значка цвета сущности, название, подпись, число справа,
// предупреждение. Основа — ui::listItem (наведение, выделение, фокус, подсказка, lastItem для меню).
bool entityRow(std::string_view title, std::string_view subtitle, const char* icon, Color tint, std::string_view hint, bool selected,
               bool warn, std::string_view tip) {
  const ui::Theme& th = ui::theme();
  RectF r = ui::next(0, 44);
  ui::at(r);
  bool clicked = ui::listItem("##row", {.subtitle = " ", .selected = selected, .tooltip = tip});
  RectF tile{r.x + 10, r.cy() - 15, 30, 30};
  ui::draw::rect(tile, tint.alpha(th.dark ? 0.2f : 0.16f), 8);
  ui::draw::icon(icon && gfx::hasIcon(icon) ? icon : "sparkles", tile.inset(7), tint);
  float x = tile.right() + 12;
  float right = r.right() - 10;
  if (warn) {
    RectF wr{right - 16, r.cy() - 8, 16, 16};
    ui::draw::icon("warning", wr, th.warning);
    right = wr.x - 8;
  }
  if (!hint.empty()) {
    float bw = std::max(22.f, ui::measure(hint, ui::Font::Caption) + 12);
    RectF br{right - bw, r.cy() - 9, bw, 18};
    ui::draw::rect(br, selected ? th.accent.alpha(0.22f) : th.surface3, 9);
    ui::draw::text(hint, br, ui::Font::Caption, selected ? th.accent : th.textDim, ui::Align::Center);
    right = br.x - 8;
  }
  float lh = ui::lineHeight(ui::Font::Body), sh = ui::lineHeight(ui::Font::Small);
  float y0 = std::round(r.cy() - (lh + sh) * 0.5f);
  ui::draw::text(title, RectF{x, y0, right - x, lh}, selected ? ui::Font::Strong : ui::Font::Body, th.text);
  ui::draw::text(subtitle, RectF{x, y0 + lh, right - x, sh}, ui::Font::Small, th.textMuted);
  return clicked;
}

// Перейти к сущности на карте: закрыть окно и выделить (после кадра).
void goTo(App& a, Selection s) {
  later(a, [s](App& x) {
    x.closeEditor();
    x.select(s, true);
  });
}

// ---- поток фишек с переносом: размер фишки — как в ui::chip (текст Small + поля, значок/точка, крестик)
namespace {
struct Flow {
  RectF area;
  float x = 0, y = 0;
  bool any = false;
};
std::vector<Flow>& flows() {
  static std::vector<Flow> f;
  return f;
}
constexpr float kChipH = 26, kChipGap = 6;
}  // namespace

void chipsBegin() {
  Flow f;
  f.area = ui::avail();
  f.x = f.area.x;
  f.y = f.area.y;
  flows().push_back(f);
}

ui::ChipAction chip(std::string_view label, const ui::ChipOpt& o) {
  if (flows().empty()) return ui::chip(label, o);
  Flow& f = flows().back();
  bool lead = o.icon || o.color.a > 0;
  float w = std::ceil(ui::measure(ui::displayText(label), ui::Font::Small)) + 20 + (lead ? 16 : 0) + (o.removable ? 18 : 0);
  w = std::min(w, f.area.w);
  if (f.any && f.x + w > f.area.right() + 0.5f) {
    f.x = f.area.x;
    f.y += kChipH + kChipGap;
  }
  ui::at(RectF{f.x, f.y, w, kChipH});
  f.x += w + kChipGap;
  f.any = true;
  return ui::chip(label, o);
}

void chipsEnd() {
  if (flows().empty()) return;
  Flow f = flows().back();
  flows().pop_back();
  if (f.any) ui::next(0, f.y + kChipH - f.area.y);   // занять место в потоке
}

// Фишки эффектов модификатора (как w::effectChips, но с переносом по ширине).
void effectChips(const Modifier& m) {
  ui::IdScope s(i64(m.id) + 0x41000000LL);
  bool any = false;
  chipsBegin();
  for (int f = 0; f < kFxCount; f++) {
    if (!m.has(Fx(f))) continue;
    double v = m.fx[size_t(f)];
    if (v == 0) continue;
    any = true;
    ui::IdScope s2{f};
    ui::ChipOpt co;
    co.icon = schema::effect(Fx(f)).icon;
    co.tone = w::effectGood(Fx(f), v) ? ui::Tone::Success : ui::Tone::Danger;
    edkit::chip(w::effectText(Fx(f), v), co);
  }
  chipsEnd();
  if (!any) ui::label("Без эффектов", {.ink = ui::Ink::Muted});
}

}  // namespace edkit
}  // namespace rg::app
