// Regnum — цвета режимов карты, вид провинций в тайлах, легенды.
#include "map/map_internal.h"

namespace rg::map::detail {

using schema::MapMode;

u64 TileStyle::key() const {
  u64 h = hash64(&mode, sizeof(mode), 0x51ed);
  h = hashMix(h, editBorders ? 0x9e37u : 0x1234u);
  u32 op = 0, dp = 0;
  std::memcpy(&op, &fillOpacity, 4);
  std::memcpy(&dp, &dpi, 4);
  h = hashMix(h, op);
  h = hashMix(h, dp);
  return h ? h : 1;
}

Color fallbackColor(Id id) { return Color::palette(int(id)); }
Color neutralColor() { return Color::hex(0xbcb6ab); }

Color contentmentColor(double t) {
  static const std::vector<Color> s{Color::hex(0xc2412f), Color::hex(0xe27b3c), Color::hex(0xe8c14a), Color::hex(0x9cc45a), Color::hex(0x3f9a54)};
  return Color::scale(s, clamp(t, 0.0, 1.0));
}
Color rebellionColor(double t) {
  static const std::vector<Color> s{Color::hex(0xf3e6b4), Color::hex(0xf2b25a), Color::hex(0xe0612f), Color::hex(0xb32222), Color::hex(0x5e0c12)};
  return Color::scale(s, clamp(t, 0.0, 1.0));
}
Color tradeColor(double t) {
  static const std::vector<Color> s{Color::hex(0xeef0cf), Color::hex(0xc9df8a), Color::hex(0x7fbf5f), Color::hex(0x2f8f5b), Color::hex(0x165a4a)};
  return Color::scale(s, clamp(t, 0.0, 1.0));
}

namespace {

Color factionColor(const World& w, Id id) {
  const Faction* f = w.faction(id);
  return f ? f->color : fallbackColor(id);
}

Color catalogColor(const std::vector<CatalogItem>& list, Id id) {
  if (!id) return neutralColor();
  const CatalogItem* c = Catalogs::find(list, id);
  return c ? c->color : neutralColor();
}

double maxTrade(const World& w, const rules::Calc& calc) {
  double m = 0;
  w.provinces.each([&](const Province& p) {
    if (p.sea) return;
    if (const rules::ProvinceCalc* pc = calc.province(p.id)) m = std::max(m, pc->tradeValue);
  });
  return m;
}

}  // namespace

std::shared_ptr<const Looks> computeLooks(const World& w, const TileStyle& s) {
  auto L = std::make_shared<Looks>();
  const MapMode m = s.mode;
  const float op = clamp(s.fillOpacity, 0.f, 1.f);
  const bool data = m == MapMode::Contentment || m == MapMode::Rebellion || m == MapMode::Trade || m == MapMode::Resources ||
                    m == MapMode::Religion || m == MapMode::Culture;
  L->fillAlpha = m == MapMode::Guilds ? op * 0.42f : data ? clamp(0.22f + op, 0.4f, 0.9f) : op;
  L->hatch = m == MapMode::Political || m == MapMode::Guilds;
  L->stateDark = data || m == MapMode::Terrain ? 1.f : 0.f;
  if (m == MapMode::Terrain) {
    L->provLineAlpha = 0.16f;
    L->seaLineAlpha = 0.3f;
    L->stateWidth = 1.2f;
    L->seamAlpha = 0.18f;
  } else if (data) {
    L->provLineAlpha = 0.3f;
    L->stateWidth = 1.5f;
  }
  if (s.editBorders) {
    L->fillAlpha *= 0.4f;
    L->provLineAlpha = 0.85f;
    L->seaLineAlpha = 0.9f;
    L->stateWidth = 1.5f;
  }

  std::shared_ptr<const rules::Calc> calc;
  if (m == MapMode::Rebellion || m == MapMode::Trade) calc = rules::calc(w);
  const double tmax = calc && m == MapMode::Trade ? maxTrade(w, *calc) : 0;
  const Catalogs& cat = *w.catalogs;

  w.provinces.each([&](const Province& p) {
    ProvLook look;
    look.land = !p.sea;
    const Faction* owner = p.owner ? w.faction(p.owner) : nullptr;
    look.state = owner && owner->isState() ? p.owner : 0;
    if (p.sea) {
      L->prov[p.id] = look;
      return;
    }
    Color c(0, 0, 0, 0);
    switch (m) {
      case MapMode::Political:
      case MapMode::Guilds:
        if (look.state) c = owner->color;
        break;
      case MapMode::Contentment: c = contentmentColor((p.contentment + 100) / 200); break;
      case MapMode::Rebellion: {
        const rules::ProvinceCalc* pc = calc ? calc->province(p.id) : nullptr;
        c = rebellionColor(pc ? pc->rebellion / 100 : 0);
        break;
      }
      case MapMode::Trade: {
        const rules::ProvinceCalc* pc = calc ? calc->province(p.id) : nullptr;
        const double v = pc ? pc->tradeValue : 0;
        c = tradeColor(tmax > 0 ? std::sqrt(v / tmax) : 0);
        break;
      }
      case MapMode::Resources:
        if (p.resource) c = catalogColor(cat.resources, p.resource);
        break;
      case MapMode::Religion: c = catalogColor(cat.religions, p.religion); break;
      case MapMode::Culture: c = catalogColor(cat.cultures, p.culture); break;
      case MapMode::Terrain:
      case MapMode::Count: break;
    }
    look.fill = c.a ? c.withA(255) : Color(0, 0, 0, 0);
    if (L->hatch && p.occupied && p.occupier && p.occupier != p.owner) look.hatch = factionColor(w, p.occupier);
    L->prov[p.id] = look;
  });

  w.factions.each([&](const Faction& f) {
    if (!f.isState()) return;
    Color line = L->stateDark > 0 ? Color(52, 48, 42, m == MapMode::Terrain ? 90 : 200) : Color::mix(f.color, Color(20, 16, 12), 0.32f).withA(235);
    L->stateLine[f.id] = line;
  });
  return L;
}

std::vector<LegendItem> legendFor(const World& w, MapMode mode) {
  std::vector<LegendItem> out;
  auto states = [&](bool guilds) {
    std::vector<const Faction*> list;
    w.factions.each([&](const Faction& f) { if (guilds ? f.isGuild() : f.isState()) list.push_back(&f); });
    std::sort(list.begin(), list.end(), [](const Faction* a, const Faction* b) { return compareRu(a->name, b->name) < 0; });
    for (const Faction* f : list) out.push_back({f->color, f->name, guilds ? "guild" : "crown"});
  };
  auto catalog = [&](const std::vector<CatalogItem>& list, auto used, const char* icon) {
    for (const CatalogItem& c : list)
      if (used(c.id)) out.push_back({c.color, c.name, icon});
  };
  auto usedBy = [&](auto field) {
    return [&w, field](Id id) {
      bool any = false;
      w.provinces.each([&](const Province& p) { any = any || (!p.sea && field(p) == id); });
      return any;
    };
  };
  switch (mode) {
    case MapMode::Political: {
      states(false);
      bool occ = false;
      w.provinces.each([&](const Province& p) { occ = occ || (p.occupied && p.occupier); });
      if (occ) out.push_back({Color::hex(0x5a5148), "Оккупация", "occupied"});
      out.push_back({Color::hex(0xffffff), "Без владельца", "land"});
      break;
    }
    case MapMode::Guilds:
      states(true);
      out.push_back({Color::hex(0xa9a59d), "Без влияния гильдий", "chart-pie"});
      if (!w.routes.empty()) out.push_back({Color::hex(0xd9a441), "Торговый маршрут", "route"});
      break;
    case MapMode::Contentment:
      out.push_back({contentmentColor(0), "Недовольство (−100)", "discontent"});
      out.push_back({contentmentColor(0.5), "Спокойствие (0)", "contentment"});
      out.push_back({contentmentColor(1), "Довольство (+100)", "contentment"});
      break;
    case MapMode::Rebellion:
      out.push_back({rebellionColor(0), "0 %", "rebellion"});
      out.push_back({rebellionColor(0.25), "25 %", "rebellion"});
      out.push_back({rebellionColor(0.5), "50 %", "rebellion"});
      out.push_back({rebellionColor(1), "100 %", "rebellion"});
      break;
    case MapMode::Trade: {
      auto calc = rules::calc(w);
      double m = maxTrade(w, *calc);
      out.push_back({tradeColor(0), "0", "trade-value"});
      if (m > 0) {
        out.push_back({tradeColor(0.5), fmtNum(m * 0.25, m < 20 ? 1 : 0), "trade-value"});
        out.push_back({tradeColor(1), fmtNum(m, m < 20 ? 1 : 0), "trade-value"});
      }
      break;
    }
    case MapMode::Resources:
      catalog(w.catalogs->resources, usedBy([](const Province& p) { return p.resource; }), "resource");
      out.push_back({Color::hex(0xffffff), "Без ресурса", "land"});
      break;
    case MapMode::Religion:
      catalog(w.catalogs->religions, usedBy([](const Province& p) { return p.religion; }), "religion");
      out.push_back({neutralColor(), "Не указана", "religion"});
      break;
    case MapMode::Culture:
      catalog(w.catalogs->cultures, usedBy([](const Province& p) { return p.culture; }), "culture");
      out.push_back({neutralColor(), "Не указана", "culture"});
      break;
    case MapMode::Terrain:
      out.push_back({Color::hex(0xffffff), "Суша", "land"});
      out.push_back({Color::hex(0x0026ff), "Море, реки и озёра", "sea"});
      out.push_back({Color::hex(0x8c8c8c), "Горы, замки и башни", "mountain"});
      break;
    case MapMode::Count: break;
  }
  return out;
}

}  // namespace rg::map::detail
