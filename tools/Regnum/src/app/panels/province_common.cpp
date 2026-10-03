// Regnum — общие помощники панели провинции: фишки владельца и оккупанта, двуполярный ползунок довольства,
// подсказки-разбивки расчётов (восстание, торговая ценность, добыча, налог), перенос фишек по строкам,
// удаление и показ провинции. Объявления повторены в province_*.cpp (общего заголовка у панели нет).
#include "app/widgets.h"
#include "gfx/text.h"

namespace rg::app::prov {

using platform::Key;

// ---------------------------------------------------------------- подписи и числа
std::string provinceTitle(const Province& p) {
  if (!p.name.empty()) return p.name;
  return p.sea ? "Морская провинция" : "Провинция без названия";
}

std::string num(double v) { return fmtNum(v, std::fabs(v - std::round(v)) > 1e-6 ? 1 : 0); }
std::string pct(double v, bool sign = false) { return fmtPct(v, std::fabs(v - std::round(v)) > 1e-6 ? 1 : 0, sign); }
std::string signedNum(double v) { return fmtSigned(v, std::fabs(v - std::round(v)) > 1e-6 ? 1 : 0); }
std::string popText(double v) { return v >= 1e6 ? fmtShort(v) : fmtNum(v); }

const char* sourceIcon(rules::EffectSource::Kind k) {
  switch (k) {
    case rules::EffectSource::Province: return "province";
    case rules::EffectSource::Faction: return "crown";
    case rules::EffectSource::Tech: return "tech";
    case rules::EffectSource::Building: return "building";
    case rules::EffectSource::Guild: return "guild";
  }
  return "sparkles";
}

const char* sourceKind(rules::EffectSource::Kind k) {
  switch (k) {
    case rules::EffectSource::Province: return "Провинция";
    case rules::EffectSource::Faction: return "Государство";
    case rules::EffectSource::Tech: return "Технология";
    case rules::EffectSource::Building: return "Постройка";
    case rules::EffectSource::Guild: return "Гильдия";
  }
  return "";
}

std::string sourceName(const World& wd, const rules::EffectSource& s) {
  switch (s.kind) {
    case rules::EffectSource::Province: return wd.provinceName(s.id);
    case rules::EffectSource::Faction:
    case rules::EffectSource::Guild: return wd.factionName(s.id);
    case rules::EffectSource::Tech:
      if (const Tech* t = wd.tech(s.id)) return t->name.empty() ? std::string("Технология") : t->name;
      return "Технология";
    case rules::EffectSource::Building:
      if (const Building* b = wd.building(s.id)) return b->name.empty() ? std::string("Постройка") : b->name;
      return "Постройка";
  }
  return {};
}

// ---------------------------------------------------------------- подсказка-разбивка
struct TipLine {
  std::string label, value;
  ui::Tone tone = ui::Tone::Neutral;
  bool total = false;
};

// Богатая подсказка к последнему элементу (у элемента должна быть своя подсказка — она заменяется этой).
void breakdown(std::string_view title, const std::vector<TipLine>& lines, float width = 300) {
  if (!ui::beginTooltip(width)) return;
  ui::label(title, {.font = ui::Font::Strong});
  ui::gap(2);
  for (size_t i = 0; i < lines.size(); i++) {
    const TipLine& l = lines[i];
    if (l.total) {
      RectF r = ui::next(5);
      ui::draw::line(r.x, r.cy(), r.right(), r.cy(), ui::theme().border, 1);
    }
    ui::Row row({ui::fr(1), ui::fr(0.55f)}, 20, 8);
    ui::label(l.label, {.font = l.total ? ui::Font::Strong : ui::Font::Small, .ink = l.total ? ui::Ink::Normal : ui::Ink::Dim});
    ui::Ink ink = l.tone == ui::Tone::Success ? ui::Ink::Success
                  : l.tone == ui::Tone::Danger ? ui::Ink::Danger
                  : l.tone == ui::Tone::Warning ? ui::Ink::Warning
                  : l.total ? ui::Ink::Normal : ui::Ink::Dim;
    ui::label(l.value, {.font = l.total ? ui::Font::Strong : ui::Font::Small, .ink = ink, .align = ui::Align::Right});
  }
  ui::endTooltip();
}

// Строки эффекта f по источникам. Процентный эффект от базы (ofBase ≥ 0) показывается вкладом в единицах
// («Торговый тракт · +10 %» → «+1,1»), без базы — процентами; количественный — числом.
void effectLines(const World& wd, const rules::Effects& fx, Fx f, bool percent, std::vector<TipLine>& out, double ofBase = -1) {
  for (const rules::EffectSource& s : fx.sources) {
    const Modifier* m = wd.modifier(s.modifier);
    if (!m || !m->has(f)) continue;
    double v = m->get(f);
    if (v == 0 || !std::isfinite(v)) continue;
    std::string who = m->name.empty() ? std::string("Модификатор") : m->name;
    if (s.kind != rules::EffectSource::Province) who += " · " + sourceName(wd, s);
    ui::Tone tone = w::effectGood(f, v) ? ui::Tone::Success : ui::Tone::Danger;
    if (percent && ofBase >= 0) out.push_back({who + " · " + pct(v, true), signedNum(ofBase * v / 100), tone});
    else out.push_back({who, percent ? pct(v, true) : signedNum(v), tone});
  }
}

std::vector<TipLine> rebellionLines(const World& wd, const Province& p, const rules::ProvinceCalc& pc) {
  std::vector<TipLine> L;
  double base = -p.contentment * schema::kRebellionPerContentment;
  L.push_back({"Довольство " + signedNum(p.contentment) + " × −0,5", pct(base, true), base > 0 ? ui::Tone::Danger : base < 0 ? ui::Tone::Success : ui::Tone::Neutral});
  size_t before = L.size();
  effectLines(wd, pc.fx, Fx::RebellionPct, true, L);
  if (L.size() == before) L.push_back({"Модификаторы", pct(0)});
  L.push_back({"Итог (0…100 %)", pct(pc.rebellion), ui::Tone::Neutral, true});
  return L;
}

std::vector<TipLine> tradeLines(const World& wd, const Province& p, const rules::ProvinceCalc& pc) {
  std::vector<TipLine> L;
  L.push_back({"Базовая ценность", num(pc.tradeBase)});
  effectLines(wd, pc.fx, Fx::TradePct, true, L, pc.tradeBase);
  if (pc.routes > 0) {
    double add = pc.tradeBase * schema::kRouteBonus * pc.routes;
    L.push_back({"Маршруты: " + fmtNum(pc.routes) + " × 10 %", signedNum(add), ui::Tone::Success});
  }
  effectLines(wd, pc.fx, Fx::TradeFlat, false, L);
  L.push_back({"Текущая ценность", num(pc.tradeValue), ui::Tone::Neutral, true});
  (void)p;
  return L;
}

std::vector<TipLine> productionLines(const World& wd, const Province& p, const rules::ProvinceCalc& pc) {
  std::vector<TipLine> L;
  L.push_back({"Количество", num(std::max(0.0, p.resourceAmount))});
  effectLines(wd, pc.fx, Fx::ResourcePct, true, L, std::max(0.0, p.resourceAmount));
  effectLines(wd, pc.fx, Fx::ResourceFlat, false, L);
  L.push_back({"Добыча за ход", num(pc.production), ui::Tone::Neutral, true});
  return L;
}

std::vector<TipLine> taxLines(const rules::ProvinceCalc& pc) {
  std::vector<TipLine> L;
  L.push_back({"Налог государства", pct(pc.taxState)});
  L.push_back({"Местный налог", pct(pc.taxLocal, true), pc.taxLocal < 0 ? ui::Tone::Success : ui::Tone::Neutral});
  bool clamped = pc.taxState + pc.taxLocal < schema::kMinTotalTax;
  if (clamped) L.push_back({"Не меньше", pct(schema::kMinTotalTax), ui::Tone::Warning});
  L.push_back({"Общий налог", pct(pc.taxTotal), ui::Tone::Neutral, true});
  return L;
}

std::vector<TipLine> slotLines(const World& wd, const rules::ProvinceCalc& pc) {
  std::vector<TipLine> L;
  L.push_back({"Величина", fmtSigned(pc.slotsSize)});
  L.push_back({"Тип города", fmtSigned(pc.slotsCity)});
  effectLines(wd, pc.fx, Fx::Slots, false, L);
  L.push_back({"Всего слотов", fmtNum(pc.slots), ui::Tone::Neutral, true});
  L.push_back({"Занято постройками", fmtNum(pc.slotsUsed), pc.slotsUsed > pc.slots ? ui::Tone::Danger : ui::Tone::Neutral});
  return L;
}

// ---------------------------------------------------------------- подсказка к области
// Невидимый элемент поверх r: даёт подсказку (и lastItem для beginTooltip).
void tipArea(RectF r, std::string_view key, std::string_view tip) {
  ui::at(r);
  std::string id = "##tip-";
  id += key;
  ui::label(id, {.font = ui::Font::Display, .tooltip = tip.empty() ? std::string_view(" ") : tip});
}

// ---------------------------------------------------------------- фишки шапки
namespace {

// Фон фишки (как ui::chip): поверхность, рамка, наведение.
void pillFrame(RectF r, ui::WidgetId wid, bool hovered) {
  const ui::Theme& t = ui::theme();
  float hv = ui::animate(wid ^ 0x9111ull, hovered ? 1.f : 0.f);
  ui::draw::rect(r, t.surface3, 13);
  ui::draw::rectStroke(r, Color::mix(t.border, t.borderStrong, hv), 13, 1);
}

RectF pillSlot(float w) {
  RectF r = ui::next(w, 26);
  if (r.h > 26) r = RectF{r.x, r.y + std::round((r.h - 26) * 0.5f), r.w, 26};
  return r;
}

}  // namespace

// Фишка государства с флажком; щелчок — true. maxW — предел ширины (название сокращается).
bool flagPill(std::string_view key, const Faction& f, std::string_view tip, float maxW) {
  const ui::Theme& t = ui::theme();
  const gfx::TextStyle& st = ui::textStyle(ui::Font::Small);
  std::string name = f.name.empty() ? std::string("Без названия") : f.name;
  const float fixed = 8 + 21 + 7 + 11;
  name = gfx::ellipsize(name, st, std::max(30.f, maxW - fixed));
  float tw = ui::measure(name, ui::Font::Small);
  RectF r = pillSlot(std::ceil(fixed + tw));
  ui::WidgetId wid = ui::id(key);
  ui::Interaction it = ui::interact(wid, r);
  pillFrame(r, wid, it.hovered);
  ui::at(RectF{r.x + 8, r.cy() - 7, 21, 14});
  ui::flag(f.flag, 21, 14, 2);
  ui::draw::text(name, RectF{r.x + 8 + 21 + 7, r.y, tw + 2, r.h}, ui::Font::Small, t.text);
  if (it.hovered) ui::setCursor(platform::Cursor::Hand);
  tipArea(r, key, tip);
  return it.clicked;
}

// Штриховка (как на карте: диагональные линии цвета оккупанта) в квадрате r.
void hatchSwatch(RectF r, Color c) {
  ui::draw::rect(r, c.alpha(0.22f), 3);
  ui::draw::pushClip(r);
  for (float k = -r.h; k < r.w; k += 4) ui::draw::line(r.x + k, r.bottom(), r.x + k + r.h, r.y, c, 1.6f);
  ui::draw::popClip();
  ui::draw::rectStroke(r, c.alpha(0.85f), 3, 1);
}

// Фишка оккупации: штриховка цветом оккупанта и его название; щелчок — true.
bool hatchPill(std::string_view key, const Faction& occ, std::string_view tip, float maxW) {
  const ui::Theme& t = ui::theme();
  const gfx::TextStyle& st = ui::textStyle(ui::Font::Small);
  std::string name = occ.name.empty() ? std::string("Оккупант") : occ.name;
  const float fixed = 8 + 14 + 7 + 11;
  name = gfx::ellipsize(name, st, std::max(30.f, maxW - fixed));
  float tw = ui::measure(name, ui::Font::Small);
  RectF r = pillSlot(std::ceil(fixed + tw));
  ui::WidgetId wid = ui::id(key);
  ui::Interaction it = ui::interact(wid, r);
  pillFrame(r, wid, it.hovered);
  hatchSwatch(RectF{r.x + 8, r.cy() - 7, 14, 14}, occ.color);
  ui::draw::text(name, RectF{r.x + 8 + 14 + 7, r.y, tw + 2, r.h}, ui::Font::Small, t.text);
  if (it.hovered) ui::setCursor(platform::Cursor::Hand);
  tipArea(r, key, tip);
  return it.clicked;
}

// ---------------------------------------------------------------- фишки с переносом строк
struct ChipSpec {
  std::string label;
  const char* icon = nullptr;
  Color color{0, 0, 0, 0};
  ui::Tone tone = ui::Tone::Neutral;
  std::string tooltip;
  bool clickable = false;
};

// Фишки в несколько строк по ширине области. Возвращает индекс нажатой (−1).
int flowChips(std::string_view key, const std::vector<ChipSpec>& chips) {
  if (chips.empty()) return -1;
  ui::IdScope scope(key);
  const float gap = 6, h = 26;
  RectF a = ui::avail();
  float W = std::max(40.f, a.w);
  std::vector<RectF> at(chips.size());
  float x = 0, y = 0;
  for (size_t i = 0; i < chips.size(); i++) {
    const ChipSpec& c = chips[i];
    bool lead = c.icon || c.color.a > 0;
    float w = std::min(W, std::ceil(ui::measure(c.label, ui::Font::Small) + 20 + (lead ? 16 : 0)));
    if (x > 0 && x + w > W) {
      x = 0;
      y += h + gap;
    }
    at[i] = RectF{x, y, w, h};
    x += w + gap;
  }
  RectF box = ui::next(y + h);
  int clicked = -1;
  for (size_t i = 0; i < chips.size(); i++) {
    const ChipSpec& c = chips[i];
    ui::IdScope s{int(i)};
    ui::at(RectF{box.x + at[i].x, box.y + at[i].y, at[i].w, h});
    ui::ChipOpt o;
    o.icon = c.icon;
    o.color = c.color;
    o.tone = c.tone;
    o.clickable = c.clickable;
    o.tooltip = c.tooltip;
    if (ui::chip(c.label, o) == ui::ChipAction::Click) clicked = int(i);
  }
  return clicked;
}

// ---------------------------------------------------------------- двуполярный ползунок
// Ползунок −mn…mx с отметкой нуля: заливка от нуля к значению (зелёная — плюс, красная — минус), шкала под дорожкой.
// Перетаскивание, щелчок по дорожке, стрелки (Shift — ×10), Home/End. Возвращает true при изменении; rectOut — область.
bool bipolarSlider(std::string_view key, double& v, double mn, double mx, bool disabled, RectF* rectOut) {
  const ui::Theme& t = ui::theme();
  RectF r = ui::next(0, 46);
  if (rectOut) *rectOut = r;
  ui::WidgetId wid = ui::id(key);
  const float valW = 46;
  RectF tr{r.x + 9, r.y + 12, std::max(20.f, r.w - 18 - valW - 6), 6};
  RectF hit{r.x, r.y, tr.right() + 9 - r.x, 30};
  ui::Interaction it{};
  if (!disabled) it = ui::interact(wid, hit, ui::IfFocusable);
  double nv = clamp(v, mn, mx);
  if (it.pressed || it.held) {
    double k = clamp(double((it.mx - tr.x) / std::max(1.f, tr.w)), 0.0, 1.0);
    nv = std::round(mn + (mx - mn) * k);
  }
  if (it.focused && !disabled) {
    auto key1 = [](Key k, u32 m) {
      if (!ui::keyPressed(k, m)) return false;
      ui::consumeKey(k);
      return true;
    };
    if (key1(Key::Right, 0) || key1(Key::Up, 0)) nv = std::min(mx, std::round(nv) + 1);
    if (key1(Key::Left, 0) || key1(Key::Down, 0)) nv = std::max(mn, std::round(nv) - 1);
    if (key1(Key::Right, platform::ModShift) || key1(Key::Up, platform::ModShift)) nv = std::min(mx, std::round(nv) + 10);
    if (key1(Key::Left, platform::ModShift) || key1(Key::Down, platform::ModShift)) nv = std::max(mn, std::round(nv) - 10);
    if (key1(Key::Home, 0)) nv = mn;
    if (key1(Key::End, 0)) nv = mx;
  }
  bool changed = nv != v;
  if (changed) v = nv;
  // Отрисовка
  float k0 = float((0 - mn) / (mx - mn)), k = float((v - mn) / (mx - mn));
  float zx = tr.x + tr.w * clamp(k0, 0.f, 1.f), vx = tr.x + tr.w * clamp(k, 0.f, 1.f);
  Color col = v > 0 ? t.success : v < 0 ? t.danger : t.textMuted;
  if (disabled) col = col.alpha(0.5f);
  ui::draw::rect(tr, t.track, 3);
  if (std::fabs(vx - zx) > 0.5f) ui::draw::gradient(RectF{std::min(zx, vx), tr.y, std::fabs(vx - zx), tr.h}, col.alpha(0.75f), col, 3, true);
  ui::draw::rect(RectF{zx - 1, tr.y - 4, 2, tr.h + 8}, t.textMuted, 1);
  float hv = ui::animate(wid ^ 0xb1b0ull, (it.hovered || it.held || it.focused) ? 1.f : 0.f);
  if (hv > 0.01f) ui::draw::circle(vx, tr.cy(), 9 + 5 * hv, col.alpha(0.18f * hv));
  ui::draw::shadow(RectF{vx - 8, tr.cy() - 8, 16, 16}, 8, 4, Color(0, 0, 0, t.dark ? 110 : 45), 1);
  ui::draw::circle(vx, tr.cy(), 8, t.dark ? Color::hex(0xf4f1ea) : Color(255, 255, 255));
  ui::draw::ring(vx, tr.cy(), 7.25f, 1.5f, col.a ? col : t.textMuted);
  // Значение и шкала
  ui::draw::text(signedNum(v), RectF{tr.right() + 6, tr.cy() - 10, valW + 3, 20}, ui::Font::Strong, v == 0 ? t.textDim : col, ui::Align::Right);
  RectF sc{tr.x - 8, tr.bottom() + 9, 32, 14};
  ui::draw::text(fmtSigned(mn), sc, ui::Font::Caption, t.textMuted, ui::Align::Left);
  ui::draw::text("0", RectF{zx - 16, sc.y, 32, 14}, ui::Font::Caption, t.textMuted, ui::Align::Center);
  ui::draw::text(fmtSigned(mx), RectF{tr.right() - 24, sc.y, 32, 14}, ui::Font::Caption, t.textMuted, ui::Align::Right);
  if (it.held) {
    std::string s = signedNum(v);
    float bw = ui::measure(s, ui::Font::Small) + 14;
    RectF b{vx - bw * 0.5f, tr.y - 30, bw, 22};
    ui::draw::shadow(b, 6, 8, t.shadow, 2);
    ui::draw::rect(b, t.dark ? Color::hex(0x2a323e) : Color::hex(0x2a2620), 6);
    ui::draw::text(s, b, ui::Font::Small, Color::hex(0xf4f1ea), ui::Align::Center);
  }
  if (it.hovered || it.held) ui::setCursor(platform::Cursor::Hand);
  return changed;
}

// ---------------------------------------------------------------- действия
void showProvince(App& a, Id pid) {
  if (!a.world().province(pid)) return;
  a.select(SelType::Province, pid, true);
}

extern const char* const kDeleteNeedsEdit;
const char* const kDeleteNeedsEdit = "Удалить провинцию можно в режиме правки границ (E): вне его границы и области закреплены";

void askDelete(App& a, Id pid) {
  const Province* p = a.world().province(pid);
  if (!p || a.readOnly() || !a.ui.editBorders) return;
  std::string name = provinceTitle(*p);
  a.confirm("Удалить провинцию?", "«" + name + "» исчезнет с карты вместе со сведениями, постройками и гарнизоном. Отменить — Ctrl+Z.",
            "Удалить", true, [pid](App& x) {
              if (!x.world().province(pid)) return;
              x.act("Удалить провинцию", [&](Tx& tx) { rules::deleteProvince(tx, pid); });
            });
}

void startMerge(App& a, Id pid) {
  if (a.readOnly()) return;
  if (a.ui.sel != Selection{SelType::Province, pid}) a.select(SelType::Province, pid);
  if (findTool(ToolId::Merge)) {
    a.setEditBorders(true);
    a.setTool(ToolId::Merge);
    a.toast("Щёлкните соседнюю провинцию — она присоединится к выбранной", ToastKind::Info, "tool-merge");
  } else {
    a.toast("Объединение — инструментом «Объединить» на панели карты (правка границ, E)", ToastKind::Info, "tool-merge");
  }
}

// Самое вероятное государство-оккупант: в войне с владельцем, иначе первое по алфавиту.
Id defaultOccupier(const World& wd, const Province& p) {
  std::vector<const Faction*> states;
  wd.factions.each([&](const Faction& f) {
    if (f.isState() && f.id != p.owner) states.push_back(&f);
  });
  std::sort(states.begin(), states.end(), [](const Faction* x, const Faction* y) { return compareRu(x->name, y->name) < 0; });
  if (p.owner)
    for (const Faction* f : states)
      if (wd.relation(p.owner, f->id).s == RelStatus::War) return f->id;
  return states.empty() ? 0 : states.front()->id;
}

}  // namespace rg::app::prov
