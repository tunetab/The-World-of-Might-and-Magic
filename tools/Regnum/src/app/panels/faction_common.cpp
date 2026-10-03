// Regnum — общие помощники панелей государств и гильдий: снимок мира кадра, вид фракции, состояние отношений,
// строки сумм, портреты, цвет и флаг новой фракции, создание и удаление с подтверждением.
#include "app/panels/faction_common.h"

#include <unordered_map>

#include "codec/jpeg.h"
#include "codec/png.h"
#include "gfx/emblems.h"
#include "gfx/flag.h"
#include "gfx/icons.h"

namespace rg::app::fac {

// ---------------------------------------------------------------- мир кадра
const World& frameWorld(App& a) {
  // Копии мира живут до начала следующего кадра (после endFrame текущего).
  static std::vector<std::unique_ptr<World>> keep;
  static u64 frame = ~0ull;
  if (ui::frameIndex() != frame) {
    keep.clear();
    frame = ui::frameIndex();
  }
  keep.push_back(std::make_unique<World>(a.world()));
  return *keep.back();
}

// ---------------------------------------------------------------- вид и названия
KindInfo kindInfo(const Faction& f) {
  if (f.isState()) return {"Государство", "crown", ui::Tone::Accent};
  if (f.stateGuild) return {"Государственная гильдия", "guild", ui::Tone::Accent};
  return {"Торговая гильдия", "guild", ui::Tone::Info};
}

std::string displayName(const Faction& f) {
  if (!f.name.empty()) return f.name;
  return f.isGuild() ? "Гильдия без названия" : "Государство без названия";
}

bool isState(App& a, Id id) {
  const Faction* f = a.world().faction(id);
  return f && f->isState();
}
bool isGuild(App& a, Id id) {
  const Faction* f = a.world().faction(id);
  return f && f->isGuild();
}

std::string resourceName(const World& w, Id res) {
  const CatalogItem* c = w.resource(res);
  return c ? (c->name.empty() ? std::string("Ресурс") : c->name) : std::string("Ресурс");
}

// ---------------------------------------------------------------- суммы
int moneyDigits(double v) {
  double a = std::fabs(v);
  return a < 100 && std::fabs(a - std::round(a)) >= 0.05 ? 1 : 0;
}
std::string money(double v) { return std::fabs(v) < 0.005 ? std::string("0") : fmtNum(v, moneyDigits(v)); }
std::string moneySigned(double v) { return std::fabs(v) < 0.005 ? std::string("0") : fmtSigned(v, moneyDigits(v)); }

// ---------------------------------------------------------------- справочники
bool catalogField(App& a, std::string_view id, rules::CatalogList list, Id value, std::string_view noneLabel, const char* undoLabel,
                  std::function<void(Tx&, Id)> assign, bool disabled) {
  const World& w = a.world();
  const auto& items = rules::catalogList(*w.catalogs, list);
  const char* defIcon = "book";
  switch (list) {
    case rules::CatalogList::Resources: defIcon = "resource"; break;
    case rules::CatalogList::Races: defIcon = "race"; break;
    case rules::CatalogList::Cultures: defIcon = "culture"; break;
    case rules::CatalogList::Religions: defIcon = "religion"; break;
    case rules::CatalogList::Governments: defIcon = "crown"; break;
    case rules::CatalogList::Positions: defIcon = "council"; break;
  }
  const bool colored = list != rules::CatalogList::Governments && list != rules::CatalogList::Positions;
  std::vector<std::string> labels;
  labels.reserve(items.size() + 1);
  for (const CatalogItem& c : items) labels.push_back(c.name.empty() ? std::string("Без названия") : c.name);
  labels.push_back("Добавить…");
  std::vector<ui::Option> opts(labels.size());
  int idx = -1;
  for (size_t i = 0; i < items.size(); i++) {
    const CatalogItem& c = items[i];
    opts[i].label = labels[i];
    opts[i].icon = !c.icon.empty() && gfx::hasIcon(c.icon) ? c.icon.c_str() : (colored ? nullptr : defIcon);
    if (colored) opts[i].color = c.color;
    if (c.id == value) idx = int(i);
  }
  opts.back().label = labels.back();
  opts.back().icon = "plus";
  ui::ComboOpt co;
  co.noneLabel = noneLabel;
  co.placeholder = noneLabel.empty() ? std::string_view("—") : noneLabel;
  co.disabled = disabled || a.readOnly();
  co.icon = defIcon;
  if (!ui::combo(id, idx, std::span<const ui::Option>(opts), co)) return false;
  if (idx == int(items.size())) {
    // Новая запись: название — запросом, затем создание и назначение одним действием.
    std::string label = undoLabel;
    a.prompt("Новая запись справочника", "Название", "", [list, label, assign](App& x, const std::string& name) {
      x.act(label, [&](Tx& tx) {
        Id nid = rules::addCatalogItem(tx, list, trim(name));
        assign(tx, nid);
      });
    });
    return false;
  }
  Id nv = idx >= 0 && idx < int(items.size()) ? items[size_t(idx)].id : 0;
  if (nv == value) return false;
  return a.act(undoLabel, [&](Tx& tx) { assign(tx, nv); });
}

// ---------------------------------------------------------------- отношения
const char* relIcon(RelStatus s) { return schema::relStatus(s).icon; }
const char* relLabel(RelStatus s) { return schema::relStatus(s).name; }

Color relColor(RelStatus s) {
  const ui::Theme& t = ui::theme();
  switch (s) {
    case RelStatus::War: return t.danger;
    case RelStatus::Alliance: return t.success;
    case RelStatus::Neutral: return t.info;
    case RelStatus::Unknown: return t.textMuted;
  }
  return t.textDim;
}

bool statusPicker(std::string_view id, RelStatus& s, bool disabled) {
  const ui::Theme& t = ui::theme();
  ui::IdScope scope(id);
  bool changed = false;
  ui::ButtonOpt bo;
  bo.variant = ui::Variant::Ghost;
  bo.size = ui::Size::Small;
  bo.fill = true;
  bo.disabled = disabled;
  bo.tooltip = "Текущее состояние отношений";
  bool clicked = ui::button("##status", bo);
  RectF r = ui::lastItem().rect;
  Color c = relColor(s);
  float k = disabled ? 0.55f : 1.f;
  ui::draw::rect(r, c.alpha((t.dark ? 0.16f : 0.12f) * k), t.radiusField);
  ui::draw::rectStroke(r, c.alpha(0.4f * k), t.radiusField, 1);
  ui::draw::icon(relIcon(s), RectF{r.x + 8, r.cy() - 7, 14, 14}, c.alpha(k));
  float tx = r.x + 27, chev = disabled ? 0.f : 20.f;
  ui::draw::text(relLabel(s), RectF{tx, r.y, std::max(0.f, r.right() - chev - 4 - tx), r.h}, ui::Font::Small, c.alpha(k));
  if (!disabled) ui::draw::icon("chevron-down", RectF{r.right() - 19, r.cy() - 6, 12, 12}, c.alpha(0.85f));
  if (clicked && !disabled) ui::openPopup("menu");
  if (ui::beginMenu("menu")) {
    for (int i = 0; i < 4; i++) {
      RelStatus st = RelStatus(i);
      ui::IdScope s2(i);
      bool pick = ui::menuItem(relLabel(st));
      RectF rr = ui::lastItem().rect;
      if (hasApp()) app().markUi("status.item." + std::to_string(i), rr);
      ui::draw::icon(relIcon(st), RectF{rr.x + 10, rr.cy() - 8, 16, 16}, relColor(st));
      if (st == s) ui::draw::icon("check", RectF{rr.right() - 26, rr.cy() - 8, 16, 16}, t.accent);
      if (pick && st != s) {
        s = st;
        changed = true;
      }
    }
    ui::next(176, 0);   // ширина меню: место для отметки справа
    ui::endMenu();
  }
  return changed;
}

// ---------------------------------------------------------------- суммы
void moneyRow(const char* icon, Color iconColor, std::string_view label, double value, std::string_view tip, bool sign, ui::Ink valueInk) {
  ui::IdScope s(label);
  ui::Row row({ui::px(18), ui::fr(1), ui::px(108)}, 26, 8);
  ui::iconColored(icon, iconColor, 16);
  ui::label(label, {.ink = ui::Ink::Dim, .tooltip = tip});
  std::string v = sign ? moneySigned(value) : money(value);
  ui::label(v, {.font = ui::Font::Strong, .ink = std::fabs(value) < 0.005 ? ui::Ink::Muted : valueInk, .align = ui::Align::Right});
}

void shareBar(std::span<const Share> parts, float height) {
  const ui::Theme& t = ui::theme();
  RectF r = ui::next(height);
  double sum = 0;
  int n = 0;
  for (const Share& p : parts)
    if (p.value > 0) {
      sum += p.value;
      n++;
    }
  if (sum <= 0) {
    ui::draw::rect(r, t.track, height * 0.5f);
    return;
  }
  const float gap = 2;
  float avail = r.w - gap * float(std::max(0, n - 1));
  float x = r.x;
  int k = 0;
  for (const Share& p : parts) {
    if (p.value <= 0) continue;
    k++;
    float w = k == n ? r.right() - x : std::max(height, std::round(avail * float(p.value / sum)));
    if (w <= 0) break;
    ui::draw::rect(RectF{x, r.y, w, height}, p.color, height * 0.5f);
    x += w + gap;
  }
}

// ---------------------------------------------------------------- рисование
void drawFlagCopy(const Flag& f, RectF r, float radius) {
  auto p = std::make_shared<Flag>(f);
  ui::custom(r, [p, radius](gfx::Canvas& c, RectF d, float s) { gfx::drawFlag(c, *p, d, radius * s, true); });
}

void tipOver(RectF r, std::string_view key, std::string_view tip) {
  if (tip.empty()) return;
  ui::at(r);
  std::string k = "##tip:";
  k += key;
  ui::label(k, {.font = ui::Font::Display, .tooltip = tip});
}

void focusRing(RectF r, float radius) {
  const ui::Theme& t = ui::theme();
  ui::draw::rectStroke(r.expand(2), t.accent, radius + 2, t.focusRing);
}

// ---------------------------------------------------------------- строка списка
RowEvents factionRow(const Faction& f, std::string_view subtitle, std::string_view hint, const char* hintIcon, bool selected, float height) {
  const ui::Theme& t = ui::theme();
  RowEvents ev;
  RectF r = ui::next(height);
  ev.rect = r;
  ui::WidgetId wid = ui::id("##frow");
  ui::Interaction it = ui::interact(wid, r, ui::IfFocusable);
  float hv = ui::animate(wid ^ 0x51ull, it.hovered ? 1.f : 0.f, 0.1f);
  const float rad = 8;
  if (selected) {
    ui::draw::rect(r, t.accent.alpha(t.dark ? 0.14f : 0.12f), rad);
    ui::draw::rect(RectF{r.x, r.y + 9, 3, r.h - 18}, t.accent, 1.5f);
  } else if (hv > 0.01f) {
    ui::draw::rect(r, (it.held ? t.pressed : t.hover).alpha(std::min(1.f, hv * 1.4f)), rad);
  }
  if (it.focused) focusRing(r, rad);
  if (it.hovered) ui::setCursor(platform::Cursor::Hand);
  // Флаг
  const float fw = 36, fh = 24;
  ui::at(RectF{r.x + 12, std::round(r.cy() - fh * 0.5f), fw, fh});
  ui::flag(f.flag, fw, fh, 3);
  float x = r.x + 12 + fw + 12;
  float right = r.right() - 12;
  if (!hint.empty()) {
    float hw = ui::measure(hint, ui::Font::Small) + 2;
    float iw = hintIcon ? 18.f : 0.f;
    float total = std::min(hw + iw, (right - x) * 0.42f);
    float hx = right - total;
    if (hintIcon) ui::draw::icon(hintIcon, RectF{hx, r.cy() - 7, 14, 14}, selected ? t.accent : t.textMuted);
    ui::draw::text(hint, RectF{hx + iw, r.y, total - iw, r.h}, ui::Font::Small, t.textMuted, ui::Align::Right);
    right = hx - 10;
  }
  const ui::Font nf = selected ? ui::Font::Strong : ui::Font::Body;
  std::string name = displayName(f);
  if (subtitle.empty()) {
    ui::draw::text(name, RectF{x, r.y, right - x, r.h}, nf, t.text);
  } else {
    float lh = ui::lineHeight(ui::Font::Body), sh = ui::lineHeight(ui::Font::Small);
    float y0 = std::round(r.cy() - (lh + sh) * 0.5f);
    ui::draw::text(name, RectF{x, y0, right - x, lh}, nf, t.text);
    ui::draw::text(subtitle, RectF{x, y0 + lh, right - x, sh}, ui::Font::Small, t.textMuted);
  }
  ev.clicked = it.clicked;
  ev.doubleClicked = it.doubleClicked;
  ev.rightClicked = it.rightClicked;
  return ev;
}

std::string factionSubtitle(const World& w, const Faction& f) {
  if (f.isState()) {
    const Character* r = w.character(f.ruler);
    std::string name = r ? (r->name.empty() ? std::string("Без имени") : r->name) : std::string();
    std::string title = !f.rulerTitle.empty() ? f.rulerTitle : (r ? r->title : std::string());
    if (name.empty()) return "Правитель не назначен";
    return title.empty() ? name : title + " " + name;
  }
  std::string home = f.homeState ? w.factionName(f.homeState) : std::string("Без государства");
  return f.stateGuild ? "Государственная · " + home : home;
}

// ---------------------------------------------------------------- портреты
const gfx::Image* portraitOf(const Character& c) {
  if (c.portrait.empty()) return nullptr;
  struct Entry {
    std::shared_ptr<gfx::Image> img;
    u64 frame = 0;
  };
  static std::unordered_map<u64, Entry> cache;
  u64 key = hash64(c.portrait);
  auto it = cache.find(key);
  if (it == cache.end()) {
    if (cache.size() > 160) {
      // Убрать давно не нужные (изображения прошлого кадра ещё могут рисоваться — оставляем их).
      for (auto i = cache.begin(); i != cache.end();) {
        if (i->second.frame + 2 < ui::frameIndex()) i = cache.erase(i);
        else ++i;
      }
    }
    Entry e;
    auto rgba = codec::decodePng(c.portrait);
    if (!rgba) rgba = codec::decodeJpeg(c.portrait);
    if (rgba && !rgba->empty()) {
      gfx::Image img = gfx::Image::fromRgba(rgba->rgba.data(), rgba->w, rgba->h);
      // Квадрат по центру и уменьшение до 96 точек.
      int side = std::min(img.w, img.h);
      gfx::Image sq(side, side);
      int ox = (img.w - side) / 2, oy = (img.h - side) / 2;
      for (int y = 0; y < side; y++) std::memcpy(sq.row(y), img.row(y + oy) + ox, size_t(side) * sizeof(u32));
      e.img = std::make_shared<gfx::Image>(side > 96 ? sq.scaled(96, 96) : std::move(sq));
    }
    it = cache.emplace(key, std::move(e)).first;
  }
  it->second.frame = ui::frameIndex();
  return it->second.img ? it->second.img.get() : nullptr;
}

Id armyOfCharacter(const World& w, Id character) {
  Id found = 0;
  w.armies.each([&](const Army& a) {
    if (found) return;
    if (a.commander == character) {
      found = a.id;
      return;
    }
    for (const ArmyGroup& g : a.groups)
      if (std::find(g.heroes.begin(), g.heroes.end(), character) != g.heroes.end()) found = a.id;
  });
  return found;
}

// ---------------------------------------------------------------- новая фракция
namespace {

// Приятные геральдические тона средней насыщенности.
constexpr u32 kNiceColors[] = {0xb8423a, 0x2f6fb5, 0x3d8f5a, 0xc9902f, 0x7a4fb0, 0x2a9d9a, 0xc4632f, 0x8c2f55, 0x5a7d2a, 0x3b4f9e,
                               0xb5577d, 0x6b8fb8, 0x9c6b3c, 0x4aa0c7, 0x7d9a3f, 0xa64fa0, 0xd0a23a, 0x4b6b4f, 0x8f3b2e, 0x5f5aa8};

double colorDist(Color a, Color b) {
  // «Красное среднее»: простая перцептивная оценка расстояния.
  double rm = (a.r + b.r) * 0.5;
  double dr = double(a.r) - b.r, dg = double(a.g) - b.g, db = double(a.b) - b.b;
  return std::sqrt((2 + rm / 256) * dr * dr + 4 * dg * dg + (2 + (255 - rm) / 256) * db * db);
}

}  // namespace

Color pickNewColor(const World& w) {
  std::vector<Color> used;
  w.factions.each([&](const Faction& f) { used.push_back(f.color); });
  Color best = Color::hex(kNiceColors[0]);
  double bestD = -1;
  for (u32 hex : kNiceColors) {
    Color c = Color::hex(hex);
    double d = 1e9;
    for (Color u : used) d = std::min(d, colorDist(c, u));
    if (d > bestD + 1e-9) {
      bestD = d;
      best = c;
    }
  }
  return best;
}

Flag defaultFlag(FactionKind kind, Color color, Id seed) {
  // Узоры, где у эмблемы однородный фон: на поле (c0) или на полосе узора (c1).
  struct Pat {
    FlagPattern p;
    bool onC1;
  };
  static const Pat statePats[] = {{FlagPattern::V3, true},   {FlagPattern::Chief, false}, {FlagPattern::Border, false},
                                  {FlagPattern::Pale, true}, {FlagPattern::H3, true},     {FlagPattern::Solid, false}};
  static const Pat guildPats[] = {{FlagPattern::Canton, true}, {FlagPattern::Pale, true}, {FlagPattern::Border, false},
                                  {FlagPattern::Chief, false}, {FlagPattern::V3, true}};
  static const char* stateEmblems[] = {"crown", "lion", "eagle", "tower", "castle", "star", "griffin", "lily", "sun", "bear", "wolf", "sword"};
  static const char* guildEmblems[] = {"key", "gem", "ship", "anchor", "wheat", "hammer", "rose", "eye"};
  bool st = kind == FactionKind::State;
  size_t np = st ? std::size(statePats) : std::size(guildPats);
  size_t ne = st ? std::size(stateEmblems) : std::size(guildEmblems);
  const Pat& pat = st ? statePats[seed % np] : guildPats[seed % np];
  const char* emb = st ? stateEmblems[(seed * 7 + 3) % ne] : guildEmblems[(seed * 5 + 1) % ne];
  Flag f;
  f.image = false;
  f.png.clear();
  f.pattern = pat.p;
  Color light = Color::hex(0xf2e3b3), dark = Color::hex(0x1d2333);
  Color c1 = color.luminance() > 0.42f ? dark : light;
  f.colors = {color, c1, color.darken(0.38f)};
  f.emblem = gfx::hasEmblem(emb) ? emb : "";
  f.emblemColor = pat.onC1 ? color.darken(0.08f) : c1;
  return f;
}

// ---------------------------------------------------------------- действия
namespace {
Id gRename = 0;
}

void requestRename(Id faction) { gRename = faction; }
bool takeRename(Id faction) {
  if (gRename != faction || faction == 0) return false;
  gRename = 0;
  return true;
}

void showTab(App& a, const char* tabId) {
  a.ui.tabOf[SelType::Faction] = tabId;
  a.requestRedraw();
}

Id createFactionUi(App& a, FactionKind kind) {
  Id nid = 0;
  bool ok = a.act(kind == FactionKind::State ? "Новое государство" : "Новая гильдия", [&](Tx& tx) {
    Color c = pickNewColor(tx.w());
    nid = rules::createFaction(tx, kind);
    Faction& f = tx.faction(nid);
    f.color = c;
    f.flag = defaultFlag(kind, c, nid);
  });
  if (!ok || !nid) return 0;
  a.select(SelType::Faction, nid);
  showTab(a, kTabOverview);
  requestRename(nid);
  return nid;
}

void confirmDelete(App& a, Id faction) {
  const Faction* f = a.world().faction(faction);
  if (!f) return;
  if (a.readOnly()) {
    a.act("Удалить", [](Tx&) {});   // покажет подсказку о прошлом ходе
    return;
  }
  bool st = f->isState();
  std::string name = displayName(*f);
  std::string text = st ? "провинции останутся без владельца, войска и флот будут распущены, сделки и отношения удалены. "
                          "Отменить можно сочетанием Ctrl+Z."
                        : "штабы и торговое влияние исчезнут, войска будут распущены, сделки и отношения удалены. "
                          "Отменить можно сочетанием Ctrl+Z.";
  a.confirm(st ? "Удалить государство?" : "Удалить гильдию?", "«" + name + "»: " + text, "Удалить", true, [faction, st, name](App& x) {
    bool ok = x.act(st ? "Удалить государство" : "Удалить гильдию", [&](Tx& tx) { rules::removeFaction(tx, faction); });
    if (!ok) return;
    if (x.ui.sel == Selection{SelType::Faction, faction}) x.clearSelection();
    x.toast(std::string(st ? "Государство «" : "Гильдия «") + name + (st ? "» удалено" : "» удалена"), ToastKind::Info, "trash", "Отменить",
            [](App& y) { y.undo(); });
  });
}

Id createStateGuildUi(App& a, Id state) {
  Id gid = 0;
  bool ok = a.act("Государственная гильдия", [&](Tx& tx) {
    Color c = pickNewColor(tx.w());
    gid = rules::createStateGuild(tx, state);
    Faction& g = tx.faction(gid);
    g.color = c;
    g.flag = defaultFlag(FactionKind::Guild, c, gid);
  });
  if (!ok || !gid) return 0;
  std::string name = a.world().factionName(gid);
  a.toast("Учреждена «" + name + "»", ToastKind::Success, "guild", "Открыть", [gid](App& x) {
    x.select(SelType::Faction, gid);
    showTab(x, kTabOverview);
  });
  return gid;
}

void openFlagEditor(App& a, Id faction) {
  if (a.readOnly()) {
    a.act("Флаг", [](Tx&) {});
    return;
  }
  if (!a.openDialog("flag", faction)) a.toast("Редактор флага недоступен", ToastKind::Warning, "flag");
}

}  // namespace rg::app::fac
