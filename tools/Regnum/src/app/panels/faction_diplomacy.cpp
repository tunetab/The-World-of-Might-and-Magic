// Regnum — вкладка «Дипломатия» (ТЗ 1.b.iv, 1.b.vi): отношения со всеми прочими фракциями (заполняется
// автоматически по списку фракций): флаг и название, значение −100…100 (поле и двуполярный индикатор),
// состояние «в войне / в союзе / статус-кво / незнакомы». Отношения симметричны (rules::setRelation):
// изменение здесь видно во вкладке другой стороны. Фильтр государств и гильдий, войны и союзы выделены.
#include "app/panels/faction_common.h"

namespace rg::app {
namespace {

using namespace fac;

void relationRow(App& a, const World& w, Id id, const rules::RelationRow& r, bool ro) {
  const ui::Theme& t = ui::theme();
  const Faction* o = w.faction(r.other);
  if (!o) return;
  const Id other = r.other;
  const bool war = r.status == RelStatus::War, ally = r.status == RelStatus::Alliance;
  const bool wide = ui::avail().w >= 520;
  const float h = wide ? 46 : 74;
  RectF rr = ui::next(h);
  a.markUi("dip.row." + std::to_string(other), rr);
  Color bg = t.dark ? t.stripe : t.surface3.alpha(0.55f);
  if (war) bg = t.danger.alpha(t.dark ? 0.09f : 0.07f);
  else if (ally) bg = t.success.alpha(t.dark ? 0.08f : 0.07f);
  ui::draw::rect(rr, bg, t.radiusCard);
  ui::draw::rectStroke(rr, war ? t.danger.alpha(0.35f) : ally ? t.success.alpha(0.32f) : (t.dark ? t.hover : t.border), t.radiusCard, 1);
  if (war || ally) ui::draw::rect(RectF{rr.x, rr.y + 10, 3, rr.h - 20}, relColor(r.status), 1.5f);
  ui::Area area(rr.inset(12, 8), 0);
  ui::gap(4);
  const std::string name = displayName(*o);
  const char* kindTip = o->isState() ? "Государство — открыть" : "Гильдия — открыть";
  auto nameCell = [&] {
    ui::label(name, {.font = ui::Font::Strong, .tooltip = kindTip});
    if (ui::lastItem().hovered) ui::setCursor(platform::Cursor::Hand);
    if (ui::lastItem().clicked) a.select(SelType::Faction, other);
  };
  auto valueField = [&] {
    double v = r.value;
    if (ui::numberField("value", v, {.min = -100, .max = 100, .step = 1, .sign = true, .disabled = ro, .tooltip = "Отношения −100…100"}))
      a.act("Отношения", [&](Tx& tx) { rules::setRelation(tx, id, other, v, r.status); },
            {.coalesce = "relation:" + std::to_string(std::min(id, other)) + ":" + std::to_string(std::max(id, other))});
    a.markUi("dip.value." + std::to_string(other));
  };
  auto statusCell = [&] {
    RelStatus s = r.status;
    if (statusPicker("status", s, ro)) a.act("Состояние отношений", [&](Tx& tx) { rules::setRelation(tx, id, other, r.value, s); });
    a.markUi("dip.status." + std::to_string(other));
  };
  if (wide) {
    ui::Row row({ui::px(30), ui::fr(1.3f), ui::fr(1), ui::px(72), ui::px(132)}, 30, 10);
    ui::flag(o->flag, 30, 20, 2.5f);
    nameCell();
    ui::meter(r.value, {.label = false});
    valueField();
    statusCell();
  } else {
    {
      ui::Row row({ui::px(30), ui::fr(1), ui::px(132)}, 26, 10);
      ui::flag(o->flag, 30, 20, 2.5f);
      nameCell();
      statusCell();
    }
    {
      ui::Row row({ui::px(30), ui::fr(1), ui::px(76)}, 28, 10);
      RectF k = ui::next(30, 28);
      ui::draw::icon(o->isState() ? "crown" : "guild", RectF{k.cx() - 7, k.cy() - 7, 14, 14}, t.textMuted);
      ui::meter(r.value, {.label = false});
      valueField();
    }
  }
}

void drawDiplomacy(App& a, Id id) {
  const World& w = frameWorld(a);
  const Faction* f = w.faction(id);
  if (!f) return;
  const bool ro = a.readOnly();
  std::vector<rules::RelationRow> all = rules::relationsOf(w, id);
  int counts[4] = {};
  for (auto& r : all) counts[int(r.status)]++;
  if (all.empty()) {
    if (ui::emptyState("diplomacy", "Других фракций пока нет.", ro ? "" : "Новое государство", "plus")) createFactionUi(a, FactionKind::State);
    return;
  }
  // Сводка состояний: метки с переносом на следующую строку, если не помещаются.
  {
    struct T {
      std::string text;
      ui::Tone tone;
      const char* icon;
    };
    std::vector<T> tags;
    if (counts[0]) tags.push_back({std::to_string(counts[0]) + " " + plural(counts[0], "война", "войны", "войн"), ui::Tone::Danger, "war"});
    if (counts[1]) tags.push_back({std::to_string(counts[1]) + " " + plural(counts[1], "союз", "союза", "союзов"), ui::Tone::Success, "alliance"});
    if (counts[2]) tags.push_back({std::to_string(counts[2]) + " статус-кво", ui::Tone::Info, "status-quo"});
    if (counts[3]) tags.push_back({std::to_string(counts[3]) + " незнакомы", ui::Tone::Neutral, "unknown"});
    float W = ui::avail().w;
    size_t i = 0;
    int line = 0;
    while (i < tags.size()) {
      ui::IdScope ls(line++);
      ui::HStack hs(24, ui::Align::Left, 6);
      float x = 0;
      for (bool first = true; i < tags.size(); first = false, i++) {
        float tw = ui::measure(tags[i].text, ui::Font::Strong) + 16 + 18;
        if (!first && x + tw > W) break;
        ui::tag(tags[i].text, tags[i].tone, tags[i].icon);
        x += tw + 6;
      }
    }
  }
  int& filter = ui::state<int>(ui::id("##dipfilter"));
  int states = 0, guilds = 0;
  for (auto& r : all)
    if (const Faction* o = w.faction(r.other)) (o->isState() ? states : guilds)++;
  std::string sAll = "Все · " + std::to_string(all.size()), sSt = "Государства · " + std::to_string(states), sGu = "Гильдии · " + std::to_string(guilds);
  ui::segmented("filter", filter, {{nullptr, sAll, "Все фракции"}, {nullptr, sSt, "Только государства"}, {nullptr, sGu, "Только гильдии"}},
                {.size = ui::Size::Small});
  a.markUi("dip.filter");
  ui::spacer(2);
  int shown = 0;
  for (const rules::RelationRow& r : all) {
    const Faction* o = w.faction(r.other);
    if (!o) continue;
    if ((filter == 1 && !o->isState()) || (filter == 2 && !o->isGuild())) continue;
    ui::IdScope sc{i64(r.other)};
    relationRow(a, w, id, r, ro);
    shown++;
  }
  if (!shown) ui::emptyState("diplomacy", filter == 2 ? "Гильдий пока нет." : "Государств, кроме этого, пока нет.");
}

// Точка на значке вкладки — фракция ведёт войну (точка не теснит соседние значки узкой полосы вкладок).
int warsBadge(App& a, Id id) {
  for (auto& r : rules::relationsOf(a.world(), id))
    if (r.status == RelStatus::War) return -1;
  return 0;
}

TabReg tab({kTabDiplomacy, "diplomacy", "Дипломатия", 40, SelType::Faction, nullptr, drawDiplomacy, warsBadge});

}  // namespace
}  // namespace rg::app
