// Regnum — вкладка фракции «Технологии» (ТЗ 1.b.v): сводка дерева (изучено, исследуется), ход исследований,
// доступные технологии и кнопка «Открыть дерево технологий» (у каждого государства и гильдии — своё дерево).
#include "app/editors/techtree.h"
#include "app/widgets.h"

namespace rg::app {

namespace {

struct T {
  Id id = 0;
  std::string name;
  int turns = 1, progress = 0;
  bool studied = false, research = false, available = false;
};

std::vector<T> techsOf(const World& w, Id faction) {
  std::vector<T> out;
  w.techs.each([&](const Tech& t) {
    if (t.faction != faction) return;
    T x;
    x.id = t.id;
    x.name = t.name.empty() ? std::string("Без названия") : t.name;
    x.turns = std::max(1, t.turns);
    x.progress = std::max(0, t.progress);
    x.studied = t.studied;
    x.research = t.research && !t.studied;
    x.available = !t.studied && !t.research && rules::canResearch(w, t.id).ok;
    out.push_back(std::move(x));
  });
  std::stable_sort(out.begin(), out.end(), [](const T& a, const T& b) { return compareRu(a.name, b.name) < 0; });
  return out;
}

void drawTech(App& a, Id fid) {
  const World& w = a.world();
  const Faction* f = w.faction(fid);
  if (!f) return;
  const bool ro = a.readOnly();
  std::vector<T> ts = techsOf(w, fid);
  if (ts.empty()) {
    ui::spacer(8);
    if (ui::emptyState("tech-tree", f->isGuild() ? "У гильдии пока нет технологий." : "Дерево технологий пусто.", "Открыть дерево технологий", "tech-tree"))
      openTechTree(a, fid);
    a.markUi("faction.tech.open", tree::emptyActionRect("Открыть дерево технологий", "tech-tree"));
    return;
  }
  int studied = 0, research = 0, avail = 0;
  for (const T& t : ts) {
    studied += t.studied;
    research += t.research;
    avail += t.available;
  }
  {
    ui::Row r({ui::fr(1), ui::fr(1)}, 64, 8);
    ui::stat(std::to_string(studied) + " / " + std::to_string(ts.size()), "Изучено", {.icon = "check-circle", .tone = ui::Tone::Success});
    ui::stat(std::to_string(research), "Исследуется", {.icon = "hourglass", .tone = ui::Tone::Info});
  }
  ui::progress(double(studied) / double(ts.size()), {.tone = ui::Tone::Success, .height = 5, .label = true});
  if (ui::button("Открыть дерево технологий", {.variant = ui::Variant::Primary, .icon = "tech-tree", .fill = true})) openTechTree(a, fid);
  a.markUi("faction.tech.open");
  if (research > 0) {
    if (ui::Section s("Исследуются", "hourglass", {.badge = std::to_string(research)}); s) {
      for (const T& t : ts) {
        if (!t.research) continue;
        ui::IdScope sc{i64(t.id)};
        ui::Row row({ui::fr(1), ui::px(30)}, ui::kAuto, 8);
        {
          ui::Group g(0, 4);
          if (ui::link(t.name)) openTechTree(a, fid, t.id);
          int left = std::max(1, t.turns - t.progress);
          ui::progress(double(t.progress) / double(t.turns),
                       {.tone = ui::Tone::Info, .height = 5, .text = std::to_string(t.progress) + "/" + std::to_string(t.turns) + " · ещё " + nTurns(left)});
        }
        Id tid = t.id;
        if (ui::iconButton("close", "Остановить исследование", {.disabled = ro})) a.act("Остановить исследование", [&](Tx& tx) { rules::stopResearch(tx, tid); });
        a.markUi("faction.tech.stop." + std::to_string(t.id));
      }
    }
  }
  if (avail > 0) {
    if (ui::Section s("Можно исследовать", "research", {.badge = std::to_string(avail)}); s) {
      for (const T& t : ts) {
        if (!t.available) continue;
        ui::IdScope sc{i64(t.id)};
        ui::Row row({ui::fr(1), ui::px(76), ui::px(30)}, 28, 8);
        if (ui::link(t.name)) openTechTree(a, fid, t.id);
        ui::label(nTurns(std::max(1, t.turns - t.progress)), {.font = ui::Font::Small, .ink = ui::Ink::Muted, .align = ui::Align::Right});
        Id tid = t.id;
        if (ui::iconButton("play", "Начать исследование", {.disabled = ro, .tone = ui::Tone::Accent}))
          a.act("Начать исследование", [&](Tx& tx) { rules::startResearch(tx, tid); });
        a.markUi("faction.tech.start." + std::to_string(t.id));
      }
    }
  }
  if (studied > 0) {
    if (ui::Section s("Изучены", "check-circle", {.defaultOpen = studied <= 12, .badge = std::to_string(studied)}); s) {
      tree::ChipFlow flow;
      for (const T& t : ts) {
        if (!t.studied) continue;
        ui::IdScope sc{i64(t.id)};
        if (tree::chip(t.name, {.icon = "check", .tone = ui::Tone::Success, .clickable = true, .tooltip = "Показать в дереве"}) == ui::ChipAction::Click)
          openTechTree(a, fid, t.id);
      }
    }
  }
}

int badgeTech(App& a, Id fid) {
  int n = 0;
  a.world().techs.each([&](const Tech& t) { n += t.faction == fid && t.research && !t.studied; });
  return n;
}

TabReg regTab({"faction.tech", "tech-tree", "Технологии", 60, SelType::Faction, nullptr, drawTech, badgeTech});

}  // namespace
}  // namespace rg::app
