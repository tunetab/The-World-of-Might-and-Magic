// Regnum — полноэкранный редактор дерева построек (ТЗ 1.h.i–ii, 1.f.ii): общее дерево (одинаковое для всех
// государств) или уникальные постройки государства. Дорожки категорий (военные, экономические, промышленные,
// жилые), карточки построек, требования к другим постройкам (связи, уровень), панель свойств с уровнями:
// срок, стоимость по ресурсам, модификаторы, описание.
#include <unordered_map>
#include <unordered_set>

#include "app/editors/buildings.h"
#include "app/editors/techtree.h"
#include "app/widgets.h"
#include "gfx/icons.h"

namespace rg::app {

// ================================================================ помощники
namespace bld {

Color catColor(BuildingCat c) {
  int i = clamp(int(c), 0, int(BuildingCat::Count) - 1);
  return Color::hex(schema::kBuildingCats[i].color);
}
const char* catIcon(BuildingCat c) { return schema::kBuildingCats[clamp(int(c), 0, int(BuildingCat::Count) - 1)].icon; }
const char* catName(BuildingCat c) { return schema::kBuildingCats[clamp(int(c), 0, int(BuildingCat::Count) - 1)].name; }

const char* iconOf(const Building& b) { return !b.icon.empty() && gfx::hasIcon(b.icon) ? b.icon.c_str() : catIcon(b.cat); }

int levelTurns(const Building& b, int level) {
  if (level < 1 || level > int(b.levels.size())) return 1;
  return std::max(1, b.levels[size_t(level - 1)].turns);
}

bool affordable(const std::map<Id, double>& cost, const Faction* payer) {
  if (!payer) return false;
  for (auto& [res, v] : cost)
    if (payer->stock(res) + 1e-9 < v) return false;
  return true;
}

void costChips(const std::map<Id, double>& cost, const Faction* payer, bool showEmpty) {
  const World& w = app().world();
  if (cost.empty()) {
    if (showEmpty) ui::label("Бесплатно", {.font = ui::Font::Small, .ink = ui::Ink::Muted, .icon = "coins"});
    return;
  }
  ui::HStack row(20, ui::Align::Left, 4);
  bool first = true;
  for (auto& [res, v] : cost) {
    ui::IdScope s{i64(res)};
    if (!first) ui::spacer(10);
    first = false;
    const CatalogItem* c = w.resource(res);
    bool lack = payer && payer->stock(res) + 1e-9 < v;
    std::string tip = (c ? c->name : std::string("Ресурс"));
    if (payer) tip += ": нужно " + fmtNum(v) + ", есть " + fmtNum(std::max(0.0, payer->stock(res)));
    ui::iconColored(w::resourceIcon(w, res), lack ? ui::theme().danger : w::resourceColor(w, res), 16, tip);
    ui::label(fmtNum(v, std::fabs(v - std::round(v)) > 1e-9 ? 1 : 0), {.font = ui::Font::Strong, .ink = lack ? ui::Ink::Danger : ui::Ink::Normal, .tooltip = tip});
  }
}

void levelEffects(const World& w, const BuildingLevel& L, bool compact) {
  ui::IdScope s("fx");
  if (!tree::effectChips(w, L.modifiers) && !compact) ui::label("Без эффектов", {.font = ui::Font::Small, .ink = ui::Ink::Muted});
}

std::string costText(const World& w, const std::map<Id, double>& cost) {
  if (cost.empty()) return "бесплатно";
  std::vector<std::string> p;
  for (auto& [res, v] : cost) {
    const CatalogItem* c = w.resource(res);
    p.push_back((c ? c->name : std::string("Ресурс")) + " " + fmtNum(v));
  }
  return join(p, ", ");
}

void iconTile(const Building& b, float size, bool dim) {
  RectF r = ui::next(size, size);
  Color c = catColor(b.cat);
  ui::draw::rect(r, c.alpha(dim ? 0.08f : 0.16f), std::round(size * 0.26f));
  ui::draw::rectStroke(r, c.alpha(dim ? 0.2f : 0.35f), std::round(size * 0.26f), 1);
  float is = std::round(size * 0.56f);
  ui::draw::icon(iconOf(b), RectF{r.cx() - is * 0.5f, r.cy() - is * 0.5f, is, is}, dim ? c.alpha(0.5f) : c);
}

}  // namespace bld

namespace {

using namespace tree;
using platform::Key;

constexpr float kBW = 232, kBH = 100;          // карточка постройки (единицы схемы)
constexpr float kHead = 30, kLanePad = 30;     // верхний и нижний отступы дорожки (подписи — в колонке слева)
constexpr float kSideW = 372;
constexpr float kGutter = 152;                // колонка подписей дорожек (точки интерфейса)
constexpr int kCats = int(BuildingCat::Count);

// Копия постройки на кадр.
struct BN {
  Id id = 0;
  Id owner = 0;
  std::string name, desc, icon;
  BuildingCat cat = BuildingCat::Economic;
  std::vector<BuildingReq> reqs;
  std::vector<BuildingLevel> levels;
  Vec2 pos;
  int built = 0, building = 0;   // провинций с постройкой: достроено / строится
  bool match = true;
};

struct LinkSel {
  Id req = 0, b = 0;   // b требует req
  explicit operator bool() const { return req && b; }
  bool operator==(const LinkSel&) const = default;
};

struct Lanes {
  double top[kCats]{}, h[kCats]{};
  int count[kCats]{};
  double total = 0;
  int at(double y) const {
    for (int c = 0; c < kCats; c++)
      if (y < top[c] + h[c]) return c;
    return kCats - 1;
  }
};

struct Ed {
  Id sel = 0;
  LinkSel link;
  Camera cam;
  Pan pan;
  bool bgDragged = false;
  Id drag = 0;
  Vec2 dragStart, dragPos;   // мировое положение карточки (левый верх) при перетаскивании
  bool dragMoved = false;
  Id conn = 0;
  bool connOut = true;
  std::string query;
  Id focusName = 0;     // технология/постройка, чьё имя получит фокус клавиатуры
  bool skipClick = false;
  bool refit = false;       // вписать дерево в следующем кадре (после правки раскладки)   // отпускание после двойного щелчка — не снимать выделение
  bool revealSel = false;
  Id menuB = 0;
  LinkSel menuLink;
  Vec2 menuAt;
  Flag flag;
};

struct Pending {
  Id owner = 0, building = 0;
  bool set = false;
};
Pending& pending() {
  static Pending p;
  return p;
}

double snap(double v) { return std::round(v / 4) * 4; }

const BN* find(const std::vector<BN>& bs, Id id) {
  for (const BN& b : bs)
    if (b.id == id) return &b;
  return nullptr;
}

Lanes lanesOf(const std::vector<BN>& bs) {
  Lanes L;
  double maxY[kCats];
  for (int c = 0; c < kCats; c++) maxY[c] = -1;
  for (const BN& b : bs) {
    int c = clamp(int(b.cat), 0, kCats - 1);
    maxY[c] = std::max(maxY[c], std::max(0.0, b.pos.y));
    L.count[c]++;
  }
  double y = 0;
  for (int c = 0; c < kCats; c++) {
    L.top[c] = y;
    L.h[c] = kHead + std::max(0.0, maxY[c]) + kBH + kLanePad;
    y += L.h[c];
  }
  L.total = y;
  return L;
}

// Левый верх карточки в координатах схемы.
Vec2 cardPos(const Lanes& L, const BN& b) {
  int c = clamp(int(b.cat), 0, kCats - 1);
  return {b.pos.x, L.top[c] + kHead + std::max(0.0, b.pos.y)};
}

Box2 boundsOf(const std::vector<BN>& bs, const Lanes& L) {
  Box2 b;
  for (const BN& x : bs) {
    Vec2 p = cardPos(L, x);
    b.add(p);
    b.add({p.x + kBW, p.y + kBH});
  }
  if (b.empty()) b = Box2{0, 0, kBW * 3, L.total};
  b.add({b.x0, 0});
  b.add({b.x1, L.total});
  return b;
}

std::string bname(const BN& b) { return b.name.empty() ? std::string("Без названия") : b.name; }

// Цикл требований: req (через свои требования) уже зависит от b.
bool wouldCycle(const World& w, Id b, Id req) {
  if (b == req) return true;
  std::vector<Id> st{req};
  std::unordered_set<Id> seen{req};
  while (!st.empty()) {
    Id id = st.back();
    st.pop_back();
    const Building* x = w.building(id);
    if (!x) continue;
    for (const BuildingReq& r : x->requires_) {
      if (r.building == b) return true;
      if (seen.insert(r.building).second) st.push_back(r.building);
    }
  }
  return false;
}

void addReq(Tx& tx, Id b, Id req) {
  const Building* x = tx.w().building(b);
  const Building* y = tx.w().building(req);
  if (!x || !y) fail("Постройка не найдена");
  if (b == req) fail("Постройка не может требовать саму себя");
  if (y->owner != 0 && y->owner != x->owner) fail("Требовать можно только общие постройки и уникальные постройки того же государства");
  for (const BuildingReq& r : x->requires_)
    if (r.building == req) return;
  if (wouldCycle(tx.w(), b, req)) fail("Связь создаст цикл требований");
  tx.building(b).requires_.push_back(BuildingReq{req, 1});
}

void removeReq(App& a, LinkSel l) {
  a.act("Удалить требование постройки", [&](Tx& tx) {
    auto& rq = tx.building(l.b).requires_;
    rq.erase(std::remove_if(rq.begin(), rq.end(), [&](const BuildingReq& r) { return r.building == l.req; }), rq.end());
  });
}

void askDelete(App& a, const BN& b) {
  std::string text = "Постройка исчезнет из дерева";
  if (b.built + b.building > 0)
    text += " и из " + std::to_string(b.built + b.building) + " " + plural(b.built + b.building, "провинции", "провинций", "провинций") +
            "; незавершённое строительство вернёт стоимость";
  text += ". Действие можно отменить Ctrl+Z.";
  Id id = b.id;
  a.confirm("Удалить постройку «" + bname(b) + "»?", text, "Удалить", true,
            [id](App& x) { x.act("Удалить постройку", [&](Tx& tx) { rules::removeBuilding(tx, id); }); });
}

Id createAt(App& a, Id owner, const Lanes& L, Vec2 center) {
  int cat = L.at(center.y);
  Vec2 p{snap(center.x - kBW * 0.5), snap(std::max(0.0, center.y - kBH * 0.5 - L.top[cat] - kHead))};
  Id nid = 0;
  if (!a.act("Новая постройка", [&](Tx& tx) {
        nid = rules::createBuilding(tx, owner, "Новая постройка");
        Building& b = tx.building(nid);
        b.cat = BuildingCat(cat);
        b.pos = p;
        b.icon = bld::catIcon(BuildingCat(cat));
      }))
    return 0;
  return nid;
}

// Авторасстановка: столбец — глубина по требованиям (внутри дерева), строки — внутри дорожки категории.
// Связанные постройки стоят по глубине (порядок в столбце — по средней строке требований, меньше пересечений);
// одиночные (без связей) заполняют свободные места дорожки слева направо — дорожки остаются невысокими.
void autoLayout(App& a, const std::vector<BN>& bs) {
  std::unordered_map<Id, int> depth;
  std::function<int(Id, int)> dep = [&](Id id, int guard) -> int {
    if (auto it = depth.find(id); it != depth.end()) return it->second;
    const BN* b = find(bs, id);
    if (!b || guard > 64) return 0;
    int d = 0;
    for (const BuildingReq& r : b->reqs)
      if (find(bs, r.building) && r.building != id) d = std::max(d, dep(r.building, guard + 1) + 1);
    depth[id] = d;
    return d;
  };
  std::unordered_set<Id> linked;
  for (const BN& b : bs) {
    dep(b.id, 0);
    for (const BuildingReq& r : b.reqs)
      if (find(bs, r.building) && r.building != b.id) {
        linked.insert(b.id);
        linked.insert(r.building);
      }
  }
  std::unordered_map<Id, Vec2> place;   // столбец, строка
  std::unordered_map<Id, int> rowOf;
  int maxDepth = 0;
  for (auto& [id, d] : depth) maxDepth = std::max(maxDepth, d);
  for (int c = 0; c < kCats; c++) {
    std::vector<const BN*> lane, single;
    for (const BN& b : bs)
      if (int(b.cat) == c) (linked.count(b.id) ? lane : single).push_back(&b);
    auto byName = [](const BN* x, const BN* y) {
      int r = compareRu(x->name, y->name);
      return r != 0 ? r < 0 : x->id < y->id;
    };
    std::sort(single.begin(), single.end(), byName);
    std::map<std::pair<int, int>, bool> used;   // (столбец, строка)
    int rows = 0;
    for (int d = 0; d <= maxDepth; d++) {
      std::vector<const BN*> col;
      for (const BN* b : lane)
        if (depth[b->id] == d) col.push_back(b);
      // Порядок: средняя строка требований этой дорожки (корни — по названию).
      auto key = [&](const BN* b) {
        double sum = 0;
        int n = 0;
        for (const BuildingReq& r : b->reqs)
          if (auto it = rowOf.find(r.building); it != rowOf.end()) {
            sum += it->second;
            n++;
          }
        return n ? sum / n : 1e9;
      };
      std::stable_sort(col.begin(), col.end(), byName);
      std::stable_sort(col.begin(), col.end(), [&](const BN* x, const BN* y) { return key(x) < key(y); });
      int k = 0;
      for (const BN* b : col) {
        place[b->id] = {double(d), double(k)};
        rowOf[b->id] = k;
        used[{d, k}] = true;
        k++;
      }
      rows = std::max(rows, k);
    }
    // Дорожка растёт вширь (холст широкий); строк больше — только если столбцов стало бы больше шести.
    int total = int(lane.size() + single.size());
    rows = std::max({rows, 1, int(std::ceil(double(total) / 6.0))});
    for (const BN* b : single) {
      for (int col = 0;; col++) {
        int row = -1;
        for (int r = 0; r < rows; r++)
          if (!used.count({col, r})) {
            row = r;
            break;
          }
        if (row < 0) continue;
        used[{col, row}] = true;
        place[b->id] = {double(col), double(row)};
        break;
      }
    }
  }
  a.act("Расставить дерево построек", [&](Tx& tx) {
    for (const BN& b : bs) {
      auto it = place.find(b.id);
      if (it == place.end() || !tx.w().building(b.id)) continue;
      Vec2 p{it->second.x * double(rules::kTreeColStep), it->second.y * double(kBH + 24)};
      if (tx.w().building(b.id)->pos != p) tx.building(b.id).pos = p;
    }
  });
}

// ---------------------------------------------------------------- сцена
struct Card {
  BN b;
  Vec2 at;
  bool sel = false, hover = false, dim = false, drop = false, dropBad = false, outHot = false, inHot = false, hasIn = false, hasOut = false, ghost = false;
};
struct EdgeD {
  Curve k;
  Color col;
  float width = 2;
  bool sel = false;
  int level = 1;
};
// Требование к постройке другого дерева (уникальная требует общую): плашка слева от карточки и стрелка во вход.
struct Ext {
  Id req = 0, b = 0;
  Id owner = 0;            // дерево требуемой постройки (0 — общее)
  RectF pill;              // мировые единицы
  Vec2 port;               // вход карточки
  std::string label, icon;
  Color col;
  bool hot = false;
};
// Плашки внешних требований карточки at (левый верх) — по одной на требование, по центру входа.
std::vector<Ext> extsOf(const World& w, const std::vector<BN>& bs, const BN& b, Vec2 at);

struct Scene {
  Camera cam;
  Ink k;
  Lanes lanes;
  int dropLane = -1;
  std::vector<Card> cards;
  std::vector<EdgeD> edges;
  std::vector<Ext> exts;
  bool pending = false;
  Curve pend;
  Color pendCol;
};

void drawCard(gfx::Canvas& c, const Scene& s, const Card& cd) {
  const Ink& k = s.k;
  const BN& b = cd.b;
  const float z = float(s.cam.z), px = 1 / z;
  RectF r{float(cd.at.x), float(cd.at.y), kBW, kBH};
  Color cc = bld::catColor(b.cat);
  c.save();
  if (cd.dim) c.setOpacity(0.28f);
  c.boxShadow(r, 12, cd.sel || cd.hover || cd.ghost ? 28 : 14, 0, k.shadow.alpha(cd.sel || cd.hover || cd.ghost ? 0.95f : 0.7f), gfx::Pt{0, cd.ghost ? 10.f : 5.f});
  gfx::Gradient g;
  g.kind = gfx::Gradient::Linear;
  g.p0 = {r.x, r.y};
  g.p1 = {r.x, r.bottom()};
  g.stops = {{0.f, Color::mix(k.surface, cc, k.dark ? 0.09f : 0.06f)}, {1.f, k.surface}};
  gfx::Paint gp;
  gp.gradient = &g;
  c.fillRoundRect(r, 12, gp);
  Color bc = cd.sel ? k.accent : cd.hover ? Color::mix(k.borderStrong, cc, 0.4f) : Color::mix(k.border, cc, 0.22f);
  c.strokeRoundRect(r.inset(0.5f * px), 12, (cd.sel ? 1.6f : 1.f) * px, bc);
  if (cd.sel) c.strokeRoundRect(r.expand(4 * px), 12 + 4 * px, 2 * px, k.accent.alpha(0.45f));
  if (cd.drop) c.strokeRoundRect(r.expand(4 * px), 12 + 4 * px, 2.2f * px, (cd.dropBad ? k.danger : k.success).alpha(0.9f));
  // Плитка значка
  RectF tile{r.x + 14, r.y + 14, 42, 42};
  c.fillRoundRect(tile, 11, cc.alpha(0.17f));
  c.strokeRoundRect(tile.inset(0.5f * px), 11, 1 * px, cc.alpha(0.35f));
  std::string ic = !b.icon.empty() && gfx::hasIcon(b.icon) ? b.icon : std::string(bld::catIcon(b.cat));
  icon(c, ic, tile.inset(10), cc);
  // Название и подпись
  text(c, bname(b), textStyle(14, gfx::FontWeight::Semibold), RectF{r.x + 66, r.y + 13, kBW - 66 - 14, 20}, k.text);
  std::string sub = std::string(bld::catName(b.cat)) + " · " + std::to_string(b.levels.size()) + " " +
                    plural(i64(b.levels.size()), "уровень", "уровня", "уровней");
  text(c, sub, textStyle(11.5f), RectF{r.x + 66, r.y + 35, kBW - 66 - 14, 16}, k.textMuted);
  // Уровни: римские цифры, построено
  float x = r.x + 14, y = r.y + 70;
  gfx::TextStyle lv = textStyle(10.5f, gfx::FontWeight::Semibold);
  int shown = std::min<int>(int(b.levels.size()), 5);
  for (int i = 0; i < shown; i++) {
    std::string rn = roman(i + 1);
    float w = std::max(22.f, gfx::measureText(rn, lv) + 12);
    RectF pr{x, y, w, 18};
    c.fillRoundRect(pr, 9, k.surfaceHi);
    c.strokeRoundRect(pr.inset(0.5f * px), 9, 1 * px, cc.alpha(0.3f));
    text(c, rn, lv, pr, k.textDim, gfx::Align::Center);
    x += w + 4;
  }
  if (int(b.levels.size()) > shown) text(c, "+" + std::to_string(b.levels.size() - size_t(shown)), lv, RectF{x, y, 30, 18}, k.textMuted);
  if (b.built + b.building > 0) {
    std::string t = std::to_string(b.built + b.building);
    gfx::TextStyle st = textStyle(11.5f, gfx::FontWeight::Semibold);
    float tw = gfx::measureText(t, st);
    RectF br{r.right() - 14 - tw - 22, y, tw + 22, 18};
    icon(c, "province", RectF{br.x, br.y + 2, 14, 14}, k.textMuted);
    text(c, t, st, RectF{br.x + 18, br.y, tw + 4, 18}, k.textDim);
  }
  // Требования к постройкам другого дерева (общие у уникальной)
  Vec2 in{r.x, r.y + kBH * 0.5}, out{r.right(), r.y + kBH * 0.5};
  if (cd.hasIn || cd.inHot || cd.hover || cd.sel) drawPort(c, in, cd.inHot ? k.accent : Color::mix(k.borderStrong, cc, 0.5f), k.bg, cd.hasIn, cd.inHot, z);
  if (cd.hasOut || cd.outHot || cd.hover || cd.sel) drawPort(c, out, cd.outHot ? k.accent : Color::mix(k.borderStrong, cc, 0.5f), k.bg, cd.hasOut, cd.outHot, z);
  c.restore();
}

void renderScene(const Scene& s, gfx::Canvas& c, RectF dev, float scale) {
  c.save();
  c.clipRoundRect(dev, 10 * scale);
  drawGrid(c, dev, scale, s.cam, s.k);
  applyCamera(c, dev, scale, s.cam);
  const float z = float(s.cam.z);
  // Дорожки категорий
  double x0 = s.cam.x - 10, x1 = s.cam.x + dev.w / (z * scale) + 10;
  double y0 = s.cam.y - 10, y1 = s.cam.y + dev.h / (z * scale) + 10;
  for (int i = 0; i < kCats; i++) {
    Color cc = bld::catColor(BuildingCat(i));
    // Первая и последняя дорожки продолжаются до краёв холста (щелчок там попадает в них же).
    double top = i == 0 ? std::min(y0, s.lanes.top[i]) : s.lanes.top[i];
    double bottom = i == kCats - 1 ? std::max(y1, s.lanes.top[i] + s.lanes.h[i]) : s.lanes.top[i] + s.lanes.h[i];
    RectF lr{float(x0), float(top), float(x1 - x0), float(bottom - top)};
    c.fillRect(lr, cc.alpha(s.dropLane == i ? 0.10f : (i % 2 ? 0.035f : 0.055f)));
    if (i > 0) c.fillRect(RectF{lr.x, float(s.lanes.top[i]), lr.w, 1.5f / z}, cc.alpha(0.35f));
  }
  for (const EdgeD& e : s.edges) {
    if (e.sel) drawCurve(c, e.k, s.k.accent.alpha(0.22f), e.width + 7, z, false);
    drawCurve(c, e.k, e.col, e.width, z, true);
    if (e.level > 1) {
      Vec2 m = curveAt(e.k, 0.5);
      std::string t = "ур. " + roman(e.level);
      gfx::TextStyle st = textStyle(10.5f, gfx::FontWeight::Semibold);
      float tw = gfx::measureText(t, st) + 12;
      RectF pr{float(m.x) - tw * 0.5f, float(m.y) - 9, tw, 18};
      c.fillRoundRect(pr, 9, s.k.surface);
      c.strokeRoundRect(pr, 9, 1 / z, e.col);
      text(c, t, st, pr, s.k.textDim, gfx::Align::Center);
    }
  }
  for (const Ext& e : s.exts) {
    Color col = e.hot ? s.k.accent : Color::mix(s.k.textMuted, e.col, 0.45f);
    drawCurve(c, curve({e.pill.right(), e.pill.y + e.pill.h * 0.5}, e.port), col.alpha(0.85f), e.hot ? 2.6f : 1.8f, z, true, true);
    c.fillRoundRect(e.pill, e.pill.h * 0.5f, s.k.surface);
    c.strokeRoundRect(e.pill.inset(0.5f / z), e.pill.h * 0.5f, (e.hot ? 1.6f : 1.f) / z, e.hot ? s.k.accent : e.col.alpha(0.55f));
    icon(c, e.icon, RectF{e.pill.x + 8, e.pill.y + (e.pill.h - 13) * 0.5f, 13, 13}, e.col);
    text(c, e.label, textStyle(11.5f), RectF{e.pill.x + 26, e.pill.y, e.pill.w - 34, e.pill.h}, e.hot ? s.k.text : s.k.textDim);
  }
  for (const Card& cd : s.cards) drawCard(c, s, cd);
  if (s.pending) drawCurve(c, s.pend, s.pendCol, 2.2f, z, true, true);
  c.restore();
}

std::vector<Ext> extsOf(const World& w, const std::vector<BN>& bs, const BN& b, Vec2 at) {
  std::vector<Ext> out;
  for (const BuildingReq& r : b.reqs) {
    if (find(bs, r.building)) continue;
    const Building* rb = w.building(r.building);
    if (!rb) continue;
    Ext e;
    e.req = r.building;
    e.b = b.id;
    e.owner = rb->owner;
    e.label = (rb->name.empty() ? std::string("Без названия") : rb->name) + (r.level > 1 ? " · " + roman(r.level) : std::string());
    e.icon = bld::iconOf(*rb);
    e.col = bld::catColor(rb->cat);
    out.push_back(std::move(e));
  }
  const double h = 24, gap = 6;
  double y0 = at.y + kBH * 0.5 - (double(out.size()) * h + double(out.size() > 0 ? out.size() - 1 : 0) * gap) * 0.5;
  gfx::TextStyle st = textStyle(11.5f);
  for (size_t i = 0; i < out.size(); i++) {
    Ext& e = out[i];
    double tw = std::min(150.0, double(gfx::measureText(e.label, st)));
    double w = tw + 26 + 12;
    e.port = {at.x, at.y + kBH * 0.5};
    e.pill = RectF{float(at.x - 40 - w), float(y0 + double(i) * (h + gap)), float(w), float(h)};
  }
  return out;
}

// ---------------------------------------------------------------- панель свойств
struct IconChoice {
  const char* name;
  const char* title;
};
const IconChoice kIcons[] = {
    {"building", "Постройка"},   {"house", "Жильё"},          {"home", "Дом"},             {"castle", "Замок"},
    {"tower", "Башня"},          {"hq", "Штаб"},              {"capital", "Столица"},      {"crown", "Корона"},
    {"coins", "Монеты"},         {"treasury", "Казна"},       {"income", "Доход"},         {"trade", "Торговля"},
    {"trade-value", "Рынок"},    {"scales", "Весы"},          {"handshake", "Сделка"},     {"guild", "Гильдия"},
    {"hammer", "Ремесло"},       {"pickaxe", "Рудник"},       {"factory", "Мастерская"},   {"build", "Стройка"},
    {"grain", "Зерно"},          {"wood", "Лес"},             {"stone", "Камень"},         {"iron", "Железо"},
    {"gem", "Самоцветы"},        {"resource", "Ресурс"},      {"army", "Войско"},          {"sword", "Меч"},
    {"swords", "Арена"},         {"shield", "Защита"},        {"bow", "Стрельбище"},       {"horse", "Конюшня"},
    {"anchor", "Гавань"},        {"fleet", "Верфь"},          {"banner", "Знамя"},         {"flag", "Флаг"},
    {"book", "Библиотека"},      {"scroll", "Архив"},         {"quill", "Писцы"},          {"research", "Лаборатория"},
    {"tech", "Наука"},           {"staff", "Посох"},          {"wand", "Магия"},           {"sparkles", "Чудо"},
    {"religion", "Храм"},        {"culture", "Культура"},     {"population", "Население"}, {"heart", "Лечебница"},
    {"b-military", "Военная"},   {"b-economic", "Экономическая"}, {"b-industrial", "Промышленная"}, {"b-residential", "Жилая"},
};

void levelCard(App& a, const BN& b, int li, bool ro) {
  const World& w = a.world();
  const BuildingLevel& L = b.levels[size_t(li)];
  ui::IdScope s(li);
  std::string title = "Уровень " + roman(li + 1);
  ui::Card card({.pad = 12, .icon = "slots", .title = title});
  ui::Disabled dis(ro);
  {
    ui::prop("Срок", "hourglass", 0.4f);
    int turns = std::max(1, L.turns);
    if (ui::numberField("turns", turns, {.min = 1, .max = 999, .unit = "ход|хода|ходов", .steppers = true}))
      a.act("Срок строительства", [&](Tx& tx) { tx.building(b.id).levels[size_t(li)].turns = std::max(1, turns); },
            {.coalesce = "bt-turns:" + std::to_string(b.id) + ":" + std::to_string(li)});
    a.markUi("bt.level." + std::to_string(li) + ".turns");
  }
  ui::caption("Стоимость");
  int ri = 0;
  for (auto [res, amount] : L.cost) {
    ui::IdScope rs(ri++);
    ui::Row row({ui::fr(1), ui::px(96), ui::px(30)}, 30, 6);
    Id nr = res;
    if (w::catalogPicker("res", rules::CatalogList::Resources, nr, "", false, ro) && nr != res) {
      Id old = res;
      double v = amount;
      a.act("Ресурс стоимости", [&](Tx& tx) {
        auto& cost = tx.building(b.id).levels[size_t(li)].cost;
        if (cost.count(nr)) fail("Этот ресурс уже есть в стоимости уровня");
        cost.erase(old);
        cost[nr] = v;
      });
    }
    double v = amount;
    if (ui::numberField("amount", v, {.min = 0, .max = 1e9, .step = 10}))
      a.act("Стоимость уровня", [&](Tx& tx) { tx.building(b.id).levels[size_t(li)].cost[res] = std::max(0.0, v); },
            {.coalesce = "bt-cost:" + std::to_string(b.id) + ":" + std::to_string(li) + ":" + std::to_string(res)});
    a.markUi("bt.level." + std::to_string(li) + ".cost." + std::to_string(res));
    if (ui::iconButton("close", "Убрать ресурс")) {
      Id r = res;
      a.act("Убрать ресурс из стоимости", [&](Tx& tx) { tx.building(b.id).levels[size_t(li)].cost.erase(r); });
    }
  }
  {
    // Следующий ресурс, которого ещё нет в стоимости (золото — первым).
    Id next = 0;
    if (!L.cost.count(kGold)) next = kGold;
    else
      for (const CatalogItem& c : w.catalogs->resources)
        if (!L.cost.count(c.id)) {
          next = c.id;
          break;
        }
    if (ui::button("Ресурс", {.variant = ui::Variant::Ghost, .icon = "plus", .size = ui::Size::Small, .disabled = next == 0,
                              .tooltip = next ? std::string_view("Добавить ресурс в стоимость уровня") : std::string_view("Все ресурсы уже в стоимости")})) {
      a.act("Ресурс в стоимости", [&](Tx& tx) { tx.building(b.id).levels[size_t(li)].cost[next] = 100; });
    }
    a.markUi("bt.level." + std::to_string(li) + ".addcost");
  }
  ui::caption("Модификаторы уровня");
  std::vector<Id> mods = L.modifiers;
  if (tree::modifierList("mods", mods, ro)) a.act("Модификаторы уровня", [&](Tx& tx) { tx.building(b.id).levels[size_t(li)].modifiers = mods; });
  a.markUi("bt.level." + std::to_string(li) + ".mods");
  bld::levelEffects(w, L, true);
  std::string desc = L.desc;
  if (ui::textArea("desc", desc, 48, {.placeholder = "Что даёт уровень", .maxLength = 400}) && desc != L.desc)
    a.act("Описание уровня", [&](Tx& tx) { tx.building(b.id).levels[size_t(li)].desc = desc; });
  // Удалить можно только последний уровень, если он нигде не построен.
  if (li == int(b.levels.size()) - 1 && b.levels.size() > 1) {
    ui::HStack hs(28, ui::Align::Right, 4);
    if (ui::iconButton("trash", "Удалить уровень " + roman(li + 1), {.size = ui::Size::Small, .tone = ui::Tone::Danger})) {
      Id bid = b.id;
      int n = int(b.levels.size());
      a.confirm("Удалить уровень " + roman(n) + "?", "Срок, стоимость и модификаторы уровня будут удалены. Действие можно отменить Ctrl+Z.", "Удалить", true,
                [bid, n](App& x) {
                  // Правило проверяет провинции и опускает требования других построек к удалённому уровню.
                  std::vector<Id> lowered;
                  bool ok = x.act("Удалить уровень постройки", [&](Tx& tx) {
                    if (int(tx.w().building(bid)->levels.size()) == n) lowered = rules::removeLastBuildingLevel(tx, bid);
                  });
                  if (ok && !lowered.empty()) {
                    std::vector<std::string> names;
                    for (Id d : lowered)
                      if (const Building* db = x.world().building(d)) names.push_back("«" + (db->name.empty() ? std::string("Без названия") : db->name) + "»");
                    x.toast("Требование к уровню " + roman(n) + " заменено на уровень " + roman(n - 1) + ": " + join(names, ", "), ToastKind::Info,
                            "building");
                  }
                });
    }
    a.markUi("bt.level." + std::to_string(li) + ".delete");
  }
}

void sideBuilding(App& a, Ed& ed, const std::vector<BN>& bs, const BN& b) {
  const World& w = a.world();
  const bool ro = a.readOnly();
  const Color none(0, 0, 0, 0);
  ui::IdScope scope{i64(b.id)};
  {
    ui::HStack hs(28, ui::Align::Left, 6);
    ui::tag(bld::catName(b.cat), ui::Tone::Neutral, bld::catIcon(b.cat));
    if (b.owner) ui::tag("Уникальная", ui::Tone::Accent, "crown");
    ui::flex();
    if (ui::iconButton("target", "Показать на схеме")) ed.revealSel = true;
    if (ui::iconButton("trash", "Удалить постройку", {.disabled = ro, .shortcut = {Key::Delete, 0}, .tone = ui::Tone::Danger})) askDelete(a, b);
    a.markUi("bt.side.delete");
  }
  {
    ui::Disabled dis(ro);
    ui::Row r({ui::px(44), ui::fr(1)}, 44, 10);
    // Значок постройки: щелчок — выбор из набора.
    {
      RectF ir = ui::next(44, 44);
      ui::Interaction it = ui::interact(ui::id("iconpick"), ir, ui::IfFocusable);
      Color cc = bld::catColor(b.cat);
      ui::draw::rect(ir, cc.alpha(it.hovered ? 0.26f : 0.17f), 11);
      ui::draw::rectStroke(ir, it.hovered ? ui::theme().accent : cc.alpha(0.4f), 11, 1);
      ui::draw::icon(!b.icon.empty() && gfx::hasIcon(b.icon) ? b.icon : std::string(bld::catIcon(b.cat)), ir.inset(11), cc);
      if (it.hovered && !ro) ui::setCursor(platform::Cursor::Hand);
      a.markUi("bt.side.icon", ir);
      if (it.clicked && !ro) ui::openPopup("icons");
    }
    if (ui::beginPopup("icons", {.width = 8 * 34 + 7 * 4 + 16})) {
      ui::caption("Значок постройки");
      ui::Row g({ui::px(34), ui::px(34), ui::px(34), ui::px(34), ui::px(34), ui::px(34), ui::px(34), ui::px(34)}, 34, 4);
      for (const IconChoice& ic : kIcons) {
        if (!gfx::hasIcon(ic.name)) continue;
        ui::IdScope s2(ic.name);
        if (ui::iconButton(ic.name, ic.title, {.toggled = b.icon == ic.name})) {
          std::string v = ic.name;
          a.act("Значок постройки", [&](Tx& tx) { tx.building(b.id).icon = v; });
          ui::closePopup();
        }
        a.markUi(std::string("bt.icon.") + ic.name);
      }
      ui::endPopup();
    }
    {
      ui::Group grp(0, 4);
      if (ed.focusName == b.id) {
        ui::setKeyboardFocus(ui::id("name"));
        ed.focusName = 0;
      }
      std::string name = b.name;
      if (ui::textField("name", name, {.placeholder = "Название постройки", .maxLength = 80, .selectAllOnFocus = true}) && !trim(name).empty() &&
          trim(name) != b.name) {
        std::string n = trim(name);
        a.act("Переименовать постройку", [&](Tx& tx) { tx.building(b.id).name = n; });
      }
      a.markUi("bt.side.name");
    }
  }
  {
    ui::Disabled dis(ro);
    ui::caption("Категория");
    int cat = int(b.cat);
    if (ui::segmented("cat", cat,
                      {{bld::catIcon(BuildingCat(0)), {}, bld::catName(BuildingCat(0))},
                       {bld::catIcon(BuildingCat(1)), {}, bld::catName(BuildingCat(1))},
                       {bld::catIcon(BuildingCat(2)), {}, bld::catName(BuildingCat(2))},
                       {bld::catIcon(BuildingCat(3)), {}, bld::catName(BuildingCat(3))}}) &&
        cat != int(b.cat))
      a.act("Категория постройки", [&](Tx& tx) { tx.building(b.id).cat = BuildingCat(cat); });
    a.markUi("bt.side.cat");
    ui::caption("Описание");
    std::string desc = b.desc;
    if (ui::textArea("desc", desc, 60, {.placeholder = "Назначение постройки", .maxLength = 600}) && desc != b.desc)
      a.act("Описание постройки", [&](Tx& tx) { tx.building(b.id).desc = desc; });
  }
  // Использование
  if (b.built + b.building > 0) {
    std::string t = "Построена в " + std::to_string(b.built) + " " + plural(b.built, "провинции", "провинциях", "провинциях");
    if (b.building) t += ", строится в " + std::to_string(b.building);
    ui::label(t, {.font = ui::Font::Small, .ink = ui::Ink::Dim, .icon = "province"});
  }
  // Требования
  if (ui::Section s("Требует построек", "link", {.badge = b.reqs.empty() ? std::string() : std::to_string(b.reqs.size())}); s) {
    if (b.reqs.empty()) ui::label("Можно строить без других построек.", {.font = ui::Font::Small, .ink = ui::Ink::Muted});
    for (const BuildingReq& rq : b.reqs) {
      const Building* rb = w.building(rq.building);
      if (!rb) continue;
      ui::IdScope s2{i64(rq.building)};
      ui::Row row({ui::fr(1), ui::px(110), ui::px(30)}, 30, 6);
      if (ui::chip(rb->name, {.icon = bld::iconOf(*rb), .color = bld::catColor(rb->cat), .clickable = true, .tooltip = "Показать"}) == ui::ChipAction::Click) {
        if (find(bs, rq.building)) {
          ed.sel = rq.building;
          ed.revealSel = true;
        } else {
          openBuildingTree(a, rb->owner, rb->id);
        }
      }
      int lvl = rq.level;
      int maxL = std::max<int>(1, int(rb->levels.size()));
      {
        ui::Disabled dis(ro || maxL <= 1);
        if (ui::numberField("lvl", lvl, {.min = 1, .max = double(maxL), .label = "ур.", .steppers = maxL > 1})) {
          Id rid = rq.building;
          a.act("Уровень требуемой постройки", [&](Tx& tx) {
            for (BuildingReq& x : tx.building(b.id).requires_)
              if (x.building == rid) x.level = clamp(lvl, 1, maxL);
          });
        }
        a.markUi("bt.req." + std::to_string(rq.building) + ".level");
      }
      if (ui::iconButton("close", "Убрать требование", {.disabled = ro})) removeReq(a, {rq.building, b.id});
    }
    // Добавить требование: общее дерево и уникальные этого государства, без циклов.
    std::vector<std::string> labels;
    std::vector<Id> ids;
    std::vector<char> bad;
    std::vector<const Building*> cand;
    w.buildings.each([&](const Building& x) {
      if (x.id == b.id) return;
      if (x.owner != 0 && x.owner != b.owner) return;
      for (const BuildingReq& r : b.reqs)
        if (r.building == x.id) return;
      cand.push_back(&x);
    });
    std::sort(cand.begin(), cand.end(), [](const Building* x, const Building* y) {
      if (x->cat != y->cat) return x->cat < y->cat;
      return compareRu(x->name, y->name) < 0;
    });
    for (const Building* x : cand) {
      ids.push_back(x->id);
      labels.push_back(x->name.empty() ? std::string("Без названия") : x->name);
      bad.push_back(wouldCycle(w, b.id, x->id) ? 1 : 0);
    }
    if (!ids.empty()) {
      std::vector<ui::Option> opts;
      for (size_t i = 0; i < ids.size(); i++)
        opts.push_back(ui::Option{labels[i], bld::iconOf(*cand[i]), none, bad[i] ? std::string_view("цикл") : std::string_view(bld::catName(cand[i]->cat)),
                                  bad[i] != 0});
      int idx = -1;
      if (ui::combo("addreq", idx, std::span<const ui::Option>(opts), {.placeholder = "Добавить требование", .icon = "plus", .disabled = ro}) && idx >= 0) {
        Id req = ids[size_t(idx)];
        a.act("Требование постройки", [&](Tx& tx) { addReq(tx, b.id, req); });
      }
      a.markUi("bt.side.addreq");
    }
  }
  // Уровни (ТЗ 1.f.ii: постройки имеют несколько уровней и улучшаются)
  {
    ui::Section s("Уровни", "slots", {.badge = std::to_string(b.levels.size()), .actionIcon = ro ? nullptr : "plus", .actionTooltip = "Добавить уровень"});
    if (s.action() && !ro) {
      a.act("Уровень постройки", [&](Tx& tx) {
        Building& m = tx.building(b.id);
        BuildingLevel nl = m.levels.empty() ? BuildingLevel{} : m.levels.back();
        nl.desc.clear();
        m.levels.push_back(nl);
      });
    }
    a.markUi("bt.side.addlevel");
    if (s) {
      if (b.levels.empty()) {
        if (ui::emptyState("slots", "У постройки нет уровней.", ro ? "" : "Добавить уровень", "plus"))
          a.act("Уровень постройки", [&](Tx& tx) { tx.building(b.id).levels.push_back(BuildingLevel{}); });
      }
      for (int li = 0; li < int(b.levels.size()); li++) levelCard(a, b, li, ro);
    }
  }
}

void sideLink(App& a, Ed& ed, const std::vector<BN>& bs) {
  const BN* req = find(bs, ed.link.req);
  const BN* b = find(bs, ed.link.b);
  if (!req || !b) return;
  const bool ro = a.readOnly();
  ui::caption("Требование");
  {
    // Требуемая → зависимая, по строке на каждую (названия бывают длинными).
    ui::Group g(0, 4);
    if (ui::chip(bname(*req) + "##req", {.color = bld::catColor(req->cat), .clickable = true, .tooltip = "Выбрать"}) == ui::ChipAction::Click) {
      ed.sel = req->id;
      ed.link = {};
      ed.revealSel = true;
    }
    {
      ui::Indent in(10);
      ui::icon("arrow-down", ui::Ink::Muted, 16);
    }
    if (ui::chip(bname(*b) + "##b", {.color = bld::catColor(b->cat), .clickable = true, .tooltip = "Выбрать"}) == ui::ChipAction::Click) {
      ed.sel = b->id;
      ed.link = {};
      ed.revealSel = true;
    }
  }
  int lvl = 1;
  for (const BuildingReq& r : b->reqs)
    if (r.building == req->id) lvl = r.level;
  int maxL = std::max<int>(1, int(req->levels.size()));
  ui::text("Для строительства «" + bname(*b) + "» в провинции нужна «" + bname(*req) + "» уровня " + roman(lvl) + " или выше.", ui::Font::Small, ui::Ink::Dim);
  {
    ui::Disabled dis(ro || maxL <= 1);
    ui::prop("Нужный уровень", "slots");
    if (ui::numberField("lvl", lvl, {.min = 1, .max = double(maxL), .steppers = true})) {
      Id bid = b->id, rid = req->id;
      a.act("Уровень требуемой постройки", [&](Tx& tx) {
        for (BuildingReq& x : tx.building(bid).requires_)
          if (x.building == rid) x.level = clamp(lvl, 1, maxL);
      });
    }
    a.markUi("bt.side.reqlevel");
  }
  if (ui::button("Удалить требование", {.variant = ui::Variant::Danger, .icon = "unlink", .fill = true, .disabled = ro, .tooltip = "Delete"})) {
    removeReq(a, ed.link);
    ed.link = {};
  }
  a.markUi("bt.side.unlink");
}

void sideOverview(App& a, Ed& ed, Id owner, const std::vector<BN>& bs, const Lanes& L) {
  const World& w = a.world();
  const Faction* f = w.faction(owner);
  {
    ui::Row r({ui::px(54), ui::fr(1)}, 40, 12);
    {
      RectF fr = ui::next(54, 40);
      if (f) {
        ui::at(RectF{fr.x, fr.y + 2, 54, 36});
        ui::flag(ed.flag, 54, 36, 5);
      } else {
        ui::draw::rect(RectF{fr.x, fr.y + 2, 54, 36}, ui::theme().accent.alpha(0.14f), 7);
        ui::draw::icon("globe", RectF{fr.x + 15, fr.y + 8, 24, 24}, ui::theme().accent);
      }
    }
    ui::Group g(0, 0);
    ui::caption(f ? "Уникальные постройки" : "Общее дерево");
    ui::label(f ? f->name : std::string("Для всех государств"), {.font = ui::Font::Subtitle});
  }
  ui::text(f ? "Постройки, доступные только провинциям этого государства — дополнение к общему дереву."
             : "Постройки общего дерева одинаковы для всех государств и доступны любой их провинции.",
           ui::Font::Small, ui::Ink::Dim);
  int built = 0;
  for (const BN& b : bs) built += b.built + b.building;
  // Постройки общего дерева (ТЗ 1.h.i: дерево государства = общее дерево + уникальные постройки). Здесь — только
  // просмотр; правка — в общем дереве.
  std::vector<const Building*> common;
  std::map<Id, int> use;   // провинций государства с постройкой
  int commonBuilt = 0;      // постройки общего дерева в провинциях государства
  if (f) {
    w.buildings.each([&](const Building& b) {
      if (b.owner == 0) common.push_back(&b);
    });
    w.provinces.each([&](const Province& p) {
      if (p.owner != owner) return;
      for (const ProvBuilding& pb : p.buildings) {
        use[pb.building]++;
        if (const Building* b = w.building(pb.building); b && b->owner == 0) commonBuilt++;
      }
    });
    std::stable_sort(common.begin(), common.end(), [](const Building* x, const Building* y) {
      if (x->cat != y->cat) return x->cat < y->cat;
      return compareRu(x->name, y->name) < 0;
    });
  }
  {
    ui::Row r({ui::fr(1), ui::fr(1)}, 64, 10);
    ui::stat(std::to_string(bs.size() + common.size()), "В дереве",
             {.icon = "building", .tone = ui::Tone::Accent,
              .tooltip = f ? "Уникальных: " + std::to_string(bs.size()) + ", общего дерева: " + std::to_string(common.size()) : std::string()});
    ui::stat(std::to_string(built + commonBuilt), "В провинциях",
             {.icon = "province", .tone = ui::Tone::Info,
              .tooltip = f ? "Уникальных: " + std::to_string(built) + ", общего дерева: " + std::to_string(commonBuilt) : std::string()});
  }
  if (f) {
    if (ui::Section s("Постройки общего дерева", "globe", {.badge = std::to_string(common.size())}); s) {
      ui::label("Доступны и этому государству. Здесь — только просмотр: щелчок открывает постройку в общем дереве.",
                {.font = ui::Font::Small, .ink = ui::Ink::Muted, .wrap = true});
      if (common.empty()) ui::label("В общем дереве пока нет построек", {.font = ui::Font::Small, .ink = ui::Ink::Muted});
      for (const Building* b : common) {
        ui::IdScope cs{i64(b->id)};
        int n = int(b->levels.size());
        std::string sub = std::string(bld::catName(b->cat)) + " · " + std::to_string(n) + " " + plural(n, "уровень", "уровня", "уровней");
        int u = use.count(b->id) ? use[b->id] : 0;
        if (ui::listItem(b->name.empty() ? std::string("Без названия") : b->name,
                         {.icon = bld::iconOf(*b), .subtitle = sub, .hint = u ? std::to_string(u) : std::string(),
                          .tooltip = "Открыть в общем дереве построек"}))
          openBuildingTree(a, 0, b->id);
        a.markUi("bt.common." + std::to_string(b->id));
      }
      if (ui::link("Изменить общее дерево", "globe")) openBuildingTree(a, 0);
      a.markUi("bt.common.edit");
    }
  }
  if (ui::Section s("Категории", "layers"); s) {
    for (int c = 0; c < kCats; c++) {
      ui::IdScope sc(c);
      ui::Row r({ui::px(22), ui::fr(1), ui::px(40)}, 26, 8);
      ui::iconColored(bld::catIcon(BuildingCat(c)), bld::catColor(BuildingCat(c)), 18);
      ui::label(bld::catName(BuildingCat(c)));
      ui::label(std::to_string(L.count[c]), {.ink = ui::Ink::Dim, .align = ui::Align::Right});
    }
  }
  if (ui::Section s("Управление", "info", {.defaultOpen = bs.size() < 3}); s) {
    ui::label("Двойной щелчок по дорожке — новая постройка этой категории", {.font = ui::Font::Small, .ink = ui::Ink::Muted, .icon = "plus", .wrap = true});
    ui::label("Перетащите карточку на другую дорожку — сменить категорию", {.font = ui::Font::Small, .ink = ui::Ink::Muted, .icon = "layers", .wrap = true});
    ui::label("Тяните от гнезда карточки к другой — требование", {.font = ui::Font::Small, .ink = ui::Ink::Muted, .icon = "link", .wrap = true});
    if (f)
      ui::label("Плашка слева от карточки — требуемая постройка общего дерева", {.font = ui::Font::Small, .ink = ui::Ink::Muted, .icon = "globe", .wrap = true});
    ui::label("Колесо — масштаб, фон — перетаскивание, F — всё дерево", {.font = ui::Font::Small, .ink = ui::Ink::Muted, .icon = "zoom-fit", .wrap = true});
  }
}

// ---------------------------------------------------------------- редактор
void drawBuildingTree(App& a, Id owner) {
  const World& w = a.world();
  const ui::Theme& th = ui::theme();
  const bool ro = a.readOnly();
  RectF R = ui::avail();
  const Faction* fac = owner ? w.faction(owner) : nullptr;
  if (owner && (!fac || !fac->isState())) {
    ui::Area ar(RectF{R.cx() - 180, R.cy() - 120, 360, 240}, 0);
    ui::emptyState("building", "Уникальные постройки бывают только у государств.");
    if (ui::button("Общее дерево построек", {.icon = "globe", .fill = true})) a.openEditor("buildings", 0);
    return;
  }
  Ed& ed = ui::state<Ed>(ui::id("bt#" + std::to_string(owner)));
  if (fac) ed.flag = fac->flag;

  // Снимок дерева на кадр.
  std::vector<BN> bs;
  std::unordered_map<Id, size_t> idx;
  w.buildings.each([&](const Building& b) {
    if (b.owner != owner) return;
    BN n;
    n.id = b.id;
    n.owner = b.owner;
    n.name = b.name;
    n.desc = b.desc;
    n.icon = b.icon;
    n.cat = BuildingCat(clamp(int(b.cat), 0, kCats - 1));
    n.reqs = b.requires_;
    n.levels = b.levels;
    n.pos = b.pos;
    n.match = ed.query.empty() || utf8::matches(b.name, ed.query) || utf8::matches(b.desc, ed.query);
    idx[b.id] = bs.size();
    bs.push_back(std::move(n));
  });
  w.provinces.each([&](const Province& p) {
    for (const ProvBuilding& pb : p.buildings)
      if (auto it = idx.find(pb.building); it != idx.end()) {
        if (pb.builtLevel() > 0) bs[it->second].built++;
        else bs[it->second].building++;
      }
  });
  if (Pending& p = pending(); p.set && p.owner == owner) {
    if (find(bs, p.building)) {
      ed.sel = p.building;
      ed.link = {};
      ed.revealSel = true;
    }
    p = {};
  }
  if (ed.sel && !find(bs, ed.sel)) ed.sel = 0;
  if (ed.link) {
    const BN* lb = find(bs, ed.link.b);
    bool ok = lb && find(bs, ed.link.req);
    if (ok) {
      ok = false;
      for (const BuildingReq& r : lb->reqs) ok = ok || r.building == ed.link.req;
    }
    if (!ok) ed.link = {};
  }
  if (ed.focusName && ed.focusName != ed.sel) ed.focusName = 0;
  if (ed.drag && !find(bs, ed.drag)) ed.drag = 0;
  if (ed.conn && !find(bs, ed.conn)) ed.conn = 0;
  Lanes L = lanesOf(bs);

  RectF bar = R.cutTop(36);
  R.cutTop(12);
  RectF side = R.cutRight(kSideW);
  R.cutRight(12);
  RectF canvas = R;
  Box2 bounds = boundsOf(bs, L);
  for (const BN& b : bs)
    for (const Ext& e : extsOf(w, bs, b, cardPos(L, b))) bounds.add(Box2{e.pill.x, e.pill.y, e.pill.right(), e.pill.bottom()});
  ed.cam.insetLeft = kGutter;
  if (!ed.cam.ready && !canvas.empty()) fit(ed.cam, canvas, bounds, false, 1.0);
  else if (ed.refit && !canvas.empty()) fit(ed.cam, canvas, bounds, true, 1.0);
  ed.refit = false;

  // ---- панель инструментов
  {
    ui::Area ar(bar, 0);
    ui::HStack hs(34, ui::Align::Left, 8);
    {
      RectF fr = ui::next(42, 34);
      if (fac) {
        ui::at(RectF{fr.x, fr.y + 3, 42, 28});
        ui::flag(ed.flag, 42, 28, 4, fac->name);
      } else {
        ui::draw::rect(RectF{fr.x, fr.y + 3, 42, 28}, th.accent.alpha(0.14f), 6);
        ui::draw::icon("globe", RectF{fr.x + 11, fr.y + 7, 20, 20}, th.accent);
      }
    }
    {
      ui::Group g(280, 0);
      ui::spacer(2);
      Id pick = owner;
      if (factionSwitch("tree", pick, true, "Общее дерево") && pick != owner) a.openEditor("buildings", pick);
      a.markUi("bt.picker");
    }
    ui::tag(std::to_string(bs.size()) + " " + plural(i64(bs.size()), "постройка", "постройки", "построек"), ui::Tone::Neutral, "building");
    if (fac) ui::tag("уникальные", ui::Tone::Accent, "crown");
    if (ro) ui::tag("Ход " + std::to_string(a.ui.viewTurn.value_or(0)) + " · только просмотр", ui::Tone::Warning, "lock");
    ui::flex();
    {
      RectF sr = ui::next(220, 34);
      ui::at(RectF{sr.x, sr.y + 2, sr.w, 30});
      std::string q = ed.query;
      if (ui::searchField("search", q, "Найти постройку")) {
        ed.query = q;
        for (const BN& b : bs)
          if (!q.empty() && (utf8::matches(b.name, q) || utf8::matches(b.desc, q))) {
            ed.sel = b.id;
            ed.link = {};
            ed.revealSel = true;
            break;
          }
      }
    }
    ui::separatorV();
    ui::Disabled d(ro);
    if (ui::iconButton("plus", "Новая постройка", {.shortcut = {Key::N, 0}})) {
      Vec2 c = toWorld(ed.cam, canvas, canvas.cx(), canvas.cy());
      if (ed.sel)
        if (const BN* s = find(bs, ed.sel)) c = {cardPos(L, *s).x + kBW * 0.5 + rules::kTreeColStep, cardPos(L, *s).y + kBH * 0.5};
      if (Id nid = createAt(a, owner, L, c)) {
        ed.sel = nid;
        ed.link = {};
        ed.focusName = ed.sel;
      }
    }
    a.markUi("bt.add");
    if (ui::iconButton("wand", "Расставить дерево автоматически", {.disabled = bs.size() < 2})) {
      autoLayout(a, bs);
      ed.refit = true;
    }
    a.markUi("bt.layout");
    ui::separatorV();
    bool canDel = ed.sel || ed.link;
    if (ui::iconButton("trash", ed.link ? "Удалить требование" : "Удалить постройку", {.disabled = !canDel, .shortcut = {Key::Delete, 0}, .tone = ui::Tone::Danger})) {
      if (ed.link) {
        removeReq(a, ed.link);
        ed.link = {};
      } else if (const BN* b = find(bs, ed.sel)) {
        askDelete(a, *b);
      }
    }
    a.markUi("bt.delete");
  }

  // ---- холст: ввод
  a.markUi("bt.canvas", canvas);
  RectF content{canvas.x + kGutter, canvas.y, std::max(0.f, canvas.w - kGutter), canvas.h};
  const ui::Mouse& mouse = ui::mouse();
  ui::Interaction bg = ui::interact(ui::id("bg"), canvas, ui::IfAllowOverlap | ui::IfMiddleButton | ui::IfRightButton);
  if (bg.pressed) ed.bgDragged = false;
  if (bg.dragging) ed.bgDragged = true;
  Vec2 mw = toWorld(ed.cam, canvas, mouse.x, mouse.y);

  Id hoverNode = 0, hoverOut = 0, hoverIn = 0;
  bool connReleased = false, openMenu = false;
  std::vector<size_t> order(bs.size());
  for (size_t i = 0; i < bs.size(); i++) order[i] = i;
  std::stable_partition(order.begin(), order.end(), [&](size_t i) { return bs[i].id != ed.sel && bs[i].id != ed.drag; });
  auto posOf = [&](const BN& b) { return ed.drag == b.id && ed.dragMoved ? ed.dragPos : cardPos(L, b); };
  for (size_t oi : order) {
    const BN& b = bs[oi];
    Vec2 p = posOf(b);
    RectF sr = toScreen(ed.cam, canvas, p.x, p.y, kBW, kBH);
    bool off = sr.right() < canvas.x - 20 || sr.x > canvas.right() + 20 || sr.bottom() < canvas.y - 20 || sr.y > canvas.bottom() + 20;
    if (off && b.id != ed.drag && b.id != ed.conn) continue;
    ui::IdScope s{i64(b.id)};
    a.markUi("bt.node." + std::to_string(b.id), sr);
    ui::Interaction ni = ui::interact(ui::id("node"), sr.intersect(content), ui::IfAllowOverlap | ui::IfRightButton);
    float ps = std::max(18.f, float(16 * ed.cam.z));
    RectF po{sr.right() - ps * 0.5f, sr.cy() - ps * 0.5f, ps, ps}, pin{sr.x - ps * 0.5f, sr.cy() - ps * 0.5f, ps, ps};
    ui::Interaction oi2 = ui::interact(ui::id("out"), po.intersect(content), ui::IfAllowOverlap);
    ui::Interaction ii = ui::interact(ui::id("in"), pin.intersect(content), ui::IfAllowOverlap);
    a.markUi("bt.out." + std::to_string(b.id), po);
    a.markUi("bt.in." + std::to_string(b.id), pin);
    if (ni.hovered) hoverNode = b.id;
    if (oi2.hovered) hoverOut = b.id;
    if (ii.hovered) hoverIn = b.id;
    if (ni.pressed) {
      ed.sel = b.id;
      ed.link = {};
      if (ni.button == 0 && !ro) {
        ed.drag = b.id;
        ed.dragStart = cardPos(L, b);
        ed.dragPos = ed.dragStart;
        ed.dragMoved = false;
      }
      if (ni.doubleClicked) ed.focusName = ed.sel;
    }
    if (ed.drag == b.id && ni.held && ni.dragging) {
      ed.dragPos = {ed.dragStart.x + double(ni.dx) / ed.cam.z, ed.dragStart.y + double(ni.dy) / ed.cam.z};
      ed.dragMoved = true;
      ui::setCursor(platform::Cursor::Grabbing);
    }
    if (ni.rightClicked) {
      ed.menuB = b.id;
      ed.menuLink = {};
      openMenu = true;
    }
    if (ni.hovered && !ed.drag && !ed.conn) ui::setCursor(platform::Cursor::Hand);
    if ((oi2.hovered || ii.hovered) && !ro) ui::setCursor(platform::Cursor::Crosshair);
    if (oi2.pressed && !ro) {
      ed.conn = b.id;
      ed.connOut = true;
    }
    if (ii.pressed && !ro) {
      ed.conn = b.id;
      ed.connOut = false;
    }
    if (ed.conn == b.id && ((ed.connOut && oi2.released) || (!ed.connOut && ii.released))) connReleased = true;
  }
  // Перенос карточки закончен — одно действие: место и, если сменилась дорожка, категория.
  int dropLane = -1;
  if (ed.drag && ed.dragMoved) dropLane = L.at(ed.dragPos.y + kBH * 0.5);
  if (ed.drag && !mouse.down[0]) {
    if (const BN* b = find(bs, ed.drag); b && ed.dragMoved) {
      int cat = clamp(L.at(ed.dragPos.y + kBH * 0.5), 0, kCats - 1);
      Vec2 np{snap(ed.dragPos.x), snap(std::max(0.0, ed.dragPos.y - L.top[cat] - kHead))};
      Id bid = b->id;
      bool catChanged = cat != int(b->cat);
      a.act(catChanged ? "Категория постройки" : "Переместить постройку", [&](Tx& tx) {
        Building& m = tx.building(bid);
        m.pos = np;
        m.cat = BuildingCat(cat);
      });
    }
    ed.drag = 0;
    ed.dragMoved = false;
  }
  // Требования к общим постройкам (у уникальных): плашки слева от карточек; щелчок — общее дерево.
  std::vector<Ext> exts;
  for (const BN& b : bs)
    for (Ext& e : extsOf(w, bs, b, posOf(b))) exts.push_back(std::move(e));
  Id openExt = 0, openExtOwner = 0;
  int hoverExt = -1;
  for (size_t i = 0; i < exts.size(); i++) {
    Ext& e = exts[i];
    RectF sr = toScreen(ed.cam, canvas, e.pill.x, e.pill.y, e.pill.w, e.pill.h);
    RectF vis = sr.intersect(content);
    if (vis.empty()) continue;
    ui::IdScope s{i64(e.b) * 1000003 + i64(e.req)};
    ui::Interaction it = ui::interact(ui::id("ext"), vis);
    a.markUi("bt.ext." + std::to_string(e.b) + "." + std::to_string(e.req), sr);
    if (it.hovered && !ed.drag && !ed.conn) {
      e.hot = true;
      hoverExt = int(i);
      ui::setCursor(platform::Cursor::Hand);
    }
    if (it.clicked && !ed.drag && !ed.conn) {
      openExt = e.req;
      openExtOwner = e.owner;
    }
  }
  // Цель связи.
  Id dropTarget = 0;
  bool dropBad = false;
  std::string dropWhy;
  if (ed.conn) {
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
      const BN& b = bs[*it];
      if (b.id == ed.conn) continue;
      Vec2 p = cardPos(L, b);
      if (mw.x >= p.x - 10 && mw.x <= p.x + kBW + 10 && mw.y >= p.y - 10 && mw.y <= p.y + kBH + 10) {
        dropTarget = b.id;
        break;
      }
    }
    if (dropTarget) {
      Id bid = ed.connOut ? dropTarget : ed.conn, req = ed.connOut ? ed.conn : dropTarget;
      const BN* tb = find(bs, bid);
      bool linked = false;
      if (tb)
        for (const BuildingReq& r : tb->reqs) linked = linked || r.building == req;
      if (linked) {
        dropBad = true;
        dropWhy = "Уже связаны";
      } else if (wouldCycle(w, bid, req)) {
        dropBad = true;
        dropWhy = "Связь создаст цикл";
      }
    }
    ui::setCursor(platform::Cursor::Crosshair);
    if (connReleased || !mouse.down[0]) {
      if (dropTarget && dropWhy != "Уже связаны") {
        Id bid = ed.connOut ? dropTarget : ed.conn, req = ed.connOut ? ed.conn : dropTarget;
        if (a.act("Требование постройки", [&](Tx& tx) { addReq(tx, bid, req); })) {
          ed.link = {req, bid};
          ed.sel = 0;
        }
      }
      ed.conn = 0;
    }
  }
  // Связь под указателем.
  LinkSel hoverLink;
  if (bg.hovered && !ed.conn && !ed.drag && !(ed.pan.on && ed.bgDragged)) {
    double best = 8 / ed.cam.z;
    for (const BN& b : bs)
      for (const BuildingReq& r : b.reqs) {
        const BN* rb = find(bs, r.building);
        if (!rb) continue;
        Vec2 pa = cardPos(L, *rb), pb = cardPos(L, b);
        double d = curveDistance(curve({pa.x + kBW, pa.y + kBH * 0.5}, {pb.x, pb.y + kBH * 0.5}), mw);
        if (d < best) {
          best = d;
          hoverLink = {r.building, b.id};
        }
      }
    if (hoverLink) ui::setCursor(platform::Cursor::Hand);
  }
  bool overCanvas = bg.hovered || hoverNode || hoverOut || hoverIn || ed.pan.on;
  panZoom(ed.cam, canvas, bg, ed.pan, overCanvas && !ed.conn && !ed.drag);
  if (bg.doubleClicked) ed.skipClick = true;
  if (bg.clicked && !ed.bgDragged && bg.button == 0) {
    if (ed.skipClick) {
      ed.skipClick = false;
    } else {
      ed.sel = 0;
      ed.link = hoverLink;
    }
  }
  if (bg.doubleClicked && !ro && !hoverLink) {
    if (Id nid = createAt(a, owner, L, mw)) {
      ed.sel = nid;
      ed.link = {};
      ed.focusName = ed.sel;
    }
  }
  if (bg.rightClicked) {
    ed.menuB = 0;
    ed.menuLink = hoverLink;
    ed.menuAt = mw;
    openMenu = true;
  }
  if (openMenu) ui::openContextMenu("ctx");
  if (ui::beginMenu("ctx")) {
    if (const BN* b = find(bs, ed.menuB)) {
      ui::menuHeader(bname(*b));
      if (ui::beginSubmenu("Категория", "layers")) {
        for (int c = 0; c < kCats; c++) {
          ui::IdScope sc(c);
          if (ui::menuItem(bld::catName(BuildingCat(c)), {.icon = bld::catIcon(BuildingCat(c)), .checked = int(b->cat) == c, .disabled = ro})) {
            Id bid = b->id;
            a.act("Категория постройки", [&](Tx& tx) { tx.building(bid).cat = BuildingCat(c); });
          }
        }
        ui::endSubmenu();
      }
      ui::menuSeparator();
      if (ui::menuItem("Удалить", {.icon = "trash", .shortcut = {Key::Delete, 0}, .danger = true, .disabled = ro})) askDelete(a, *b);
    } else if (ed.menuLink) {
      ui::menuHeader("Требование");
      if (ui::menuItem("Удалить требование", {.icon = "unlink", .shortcut = {Key::Delete, 0}, .danger = true, .disabled = ro})) removeReq(a, ed.menuLink);
    } else {
      int lane = L.at(ed.menuAt.y);
      if (ui::menuItem(std::string("Новая постройка: ") + bld::catName(BuildingCat(lane)), {.icon = "plus", .disabled = ro})) {
        if (Id nid = createAt(a, owner, L, ed.menuAt)) {
          ed.sel = nid;
          ed.focusName = ed.sel;
        }
      }
      if (ui::menuItem("Расставить автоматически", {.icon = "wand", .disabled = ro || bs.size() < 2})) {
        autoLayout(a, bs);
        ed.refit = true;
      }
      if (ui::menuItem("Показать всё дерево", {.icon = "zoom-fit", .shortcut = {Key::F, 0}})) fit(ed.cam, canvas, bounds, true);
    }
    ui::endMenu();
  }

  // ---- клавиатура
  if (!ui::anyModalOpen()) {
    if (ui::shortcut({Key::F, 0})) fit(ed.cam, canvas, bounds, true);
    if (ui::shortcut({Key::Equal, 0}) || ui::shortcut({Key::NumAdd, 0})) zoomCenter(ed.cam, canvas, 1.25);
    if (ui::shortcut({Key::Minus, 0}) || ui::shortcut({Key::NumSub, 0})) zoomCenter(ed.cam, canvas, 1 / 1.25);
    if (!ro && (ui::shortcut({Key::Delete, 0}) || ui::shortcut({Key::Backspace, 0}))) {
      if (ed.link) {
        removeReq(a, ed.link);
        ed.link = {};
      } else if (const BN* b = find(bs, ed.sel)) {
        askDelete(a, *b);
      }
    }
  }
  if (ed.revealSel) {
    if (const BN* b = find(bs, ed.sel)) {
      Vec2 p = cardPos(L, *b);
      reveal(ed.cam, canvas, Box2{p.x, p.y, p.x + kBW, p.y + kBH}, true);
    }
    ed.revealSel = false;
  }

  // ---- сцена
  auto sc = std::make_shared<Scene>();
  sc->cam = ed.cam;
  sc->k = ink();
  sc->lanes = L;
  sc->dropLane = dropLane;
  sc->exts = exts;
  for (const BN& b : bs)
    for (const BuildingReq& r : b.reqs) {
      const BN* rb = find(bs, r.building);
      if (!rb) continue;
      Vec2 pa = posOf(*rb), pb = posOf(b);
      EdgeD e;
      e.k = curve({pa.x + kBW, pa.y + kBH * 0.5}, {pb.x, pb.y + kBH * 0.5});
      LinkSel ls{r.building, b.id};
      e.sel = ed.link == ls;
      Color base = Color::mix(sc->k.textMuted, bld::catColor(rb->cat), 0.45f).alpha(0.85f);
      if (!b.match || !rb->match) base = base.alpha(0.3f);
      e.col = e.sel ? sc->k.accent : hoverLink == ls ? base.lighten(0.3f).alpha(1.f) : base;
      e.width = e.sel || hoverLink == ls ? 3.f : 2.f;
      e.level = r.level;
      sc->edges.push_back(e);
    }
  for (size_t oi : order) {
    const BN& b = bs[oi];
    Card cd;
    cd.b = b;
    cd.at = posOf(b);
    cd.sel = ed.sel == b.id;
    cd.hover = hoverNode == b.id || hoverOut == b.id || hoverIn == b.id;
    cd.ghost = ed.drag == b.id && ed.dragMoved;
    cd.dim = !b.match;
    cd.drop = dropTarget == b.id;
    cd.dropBad = dropBad;
    cd.outHot = hoverOut == b.id || (ed.conn == b.id && ed.connOut);
    cd.inHot = hoverIn == b.id || (ed.conn == b.id && !ed.connOut);
    cd.hasIn = !b.reqs.empty();
    for (const BN& o : bs)
      for (const BuildingReq& r : o.reqs) cd.hasOut = cd.hasOut || r.building == b.id;
    sc->cards.push_back(std::move(cd));
  }
  if (ed.conn) {
    if (const BN* src = find(bs, ed.conn)) {
      sc->pending = true;
      Vec2 p = cardPos(L, *src);
      Vec2 from = ed.connOut ? Vec2{p.x + kBW, p.y + kBH * 0.5} : Vec2{p.x, p.y + kBH * 0.5};
      Vec2 to = mw;
      if (const BN* tg = find(bs, dropTarget); tg && !dropBad) {
        Vec2 q = cardPos(L, *tg);
        to = ed.connOut ? Vec2{q.x, q.y + kBH * 0.5} : Vec2{q.x + kBW, q.y + kBH * 0.5};
      }
      sc->pend = ed.connOut ? curve(from, to) : curve(to, from);
      sc->pendCol = dropTarget ? (dropBad ? sc->k.danger : sc->k.success) : sc->k.accent;
    }
  }
  ui::custom(canvas, [sc](gfx::Canvas& c, RectF dev, float scale) { renderScene(*sc, c, dev, scale); });

  // ---- поверх холста
  ui::draw::pushClip(canvas);
  // Колонка подписей дорожек (закреплена у левого края холста).
  {
    RectF gut{canvas.x, canvas.y, kGutter, canvas.h};
    ui::draw::rect(gut, th.surface2, 10);
    ui::draw::rect(RectF{gut.right() - 12, gut.y, 12, gut.h}, th.surface2);
    ui::draw::line(gut.right(), gut.y, gut.right(), gut.bottom(), th.border, 1);
    ui::draw::pushClip(gut);
    for (int c = 0; c < kCats; c++) {
      float y0 = toScreen(ed.cam, canvas, {0, L.top[c]}).y, y1 = toScreen(ed.cam, canvas, {0, L.top[c] + L.h[c]}).y;
      // Полоса: первая и последняя дорожки — до краёв холста; подпись — по центру видимой части самой дорожки.
      float by0 = c == 0 ? std::min(y0, canvas.y) : y0, by1 = c == kCats - 1 ? std::max(y1, canvas.bottom()) : y1;
      float bv0 = std::max(by0, canvas.y), bv1 = std::min(by1, canvas.bottom());
      if (bv1 - bv0 < 2) continue;
      Color cc = bld::catColor(BuildingCat(c));
      ui::draw::rect(RectF{gut.x, bv0, gut.w, bv1 - bv0}, cc.alpha(dropLane == c ? 0.16f : 0.07f));
      ui::draw::rect(RectF{gut.x, bv0, 3, bv1 - bv0}, cc.alpha(0.8f));
      if (c > 0 && y0 >= canvas.y) ui::draw::line(gut.x, y0, gut.right(), y0, cc.alpha(0.35f), 1);
      float vy0 = std::max(y0, canvas.y), vy1 = std::min(y1, canvas.bottom());
      if (vy1 - vy0 < 2) {
        vy0 = bv0;
        vy1 = bv1;
      }
      std::string cnt = std::to_string(L.count[c]);
      float cy = std::round((vy0 + vy1) * 0.5f);
      bool compact = vy1 - vy0 < 46;
      if (compact) {
        ui::draw::icon(bld::catIcon(BuildingCat(c)), RectF{gut.x + 14, cy - 9, 18, 18}, cc);
        ui::draw::text(bld::catName(BuildingCat(c)), RectF{gut.x + 38, cy - 9, gut.w - 46, 18}, ui::Font::Small, th.textDim);
      } else {
        ui::draw::rect(RectF{gut.x + 14, cy - 23, 30, 30}, cc.alpha(0.16f), 8);
        ui::draw::icon(bld::catIcon(BuildingCat(c)), RectF{gut.x + 20, cy - 17, 18, 18}, cc);
        ui::draw::text(cnt, RectF{gut.x + 52, cy - 23, gut.w - 60, 30}, ui::Font::Number, th.text);
        ui::draw::text(bld::catName(BuildingCat(c)), RectF{gut.x + 14, cy + 10, gut.w - 22, 18}, ui::Font::Strong, th.text);
      }
    }
    ui::draw::popClip();
  }
  ui::draw::rectStroke(canvas, th.border, 10, 1);
  if (bs.empty()) {
    if (canvasEmpty(canvas, "building", fac ? "Уникальных построек пока нет." : "В общем дереве пока нет построек.", "Новая постройка", "plus", ro, {},
                    nullptr, "bt.empty")) {
      if (Id nid = createAt(a, owner, L, {kBW * 0.5, L.top[1] + kHead + kBH * 0.5})) {
        ed.sel = nid;
        ed.focusName = ed.sel;
        ed.refit = true;
      }
    }
  }
  if (ed.link && !ro) {
    const BN* rb = find(bs, ed.link.req);
    const BN* b = find(bs, ed.link.b);
    if (rb && b) {
      Vec2 pa = cardPos(L, *rb), pb = cardPos(L, *b);
      Vec2 mid = curveAt(curve({pa.x + kBW, pa.y + kBH * 0.5}, {pb.x, pb.y + kBH * 0.5}), 0.5);
      gfx::Pt sp = toScreen(ed.cam, canvas, mid);
      sp.y += 26;   // под подписью уровня
      if (canvas.inset(14).contains(sp.x, sp.y)) {
        RectF br{std::round(sp.x - 15), std::round(sp.y - 15), 30, 30};
        ui::draw::shadow(br, 15, 12, th.shadow, 3);
        ui::draw::rect(br, th.surface1, 15);
        ui::draw::rectStroke(br, th.danger.alpha(0.6f), 15, 1.2f);
        ui::at(br);
        if (ui::iconButton("unlink", "Удалить требование", {.shortcut = {Key::Delete, 0}, .tone = ui::Tone::Danger})) {
          removeReq(a, ed.link);
          ed.link = {};
        }
        a.markUi("bt.link.delete");
      }
    }
  }
  if (ed.conn && dropBad && !dropWhy.empty()) {
    float tw = ui::measure(dropWhy, ui::Font::Small) + 30;
    RectF r{mouse.x + 14, mouse.y + 14, tw, 24};
    ui::draw::rect(r, th.surface1, 12);
    ui::draw::rectStroke(r, th.danger.alpha(0.7f), 12, 1);
    ui::draw::icon("warning", RectF{r.x + 8, r.y + 5, 14, 14}, th.danger);
    ui::draw::text(dropWhy, RectF{r.x + 25, r.y, tw - 28, r.h}, ui::Font::Small, th.danger);
  }
  if (!bs.empty()) {
    zoomBar(a, ed.cam, canvas, bounds, "bt.zoom");
    std::vector<MiniItem> mi;
    for (const BN& b : bs) {
      Vec2 p = cardPos(L, b);
      mi.push_back({Box2{p.x, p.y, p.x + kBW, p.y + kBH}, bld::catColor(b.cat).alpha(b.match ? 0.9f : 0.3f), b.id == ed.sel});
    }
    minimap(ed.cam, canvas, bounds, mi, "bt.mini");
  }
  ui::draw::popClip();

  // ---- панель свойств
  {
    ui::draw::rect(side, th.surface2, th.radiusCard);
    ui::draw::rectStroke(side, th.border, th.radiusCard, 1);
    ui::Area ar(side.inset(14, 12), 0);
    ui::Scroll scroll("side");
    if (const BN* b = find(bs, ed.sel)) sideBuilding(a, ed, bs, *b);
    else if (ed.link) sideLink(a, ed, bs);
    else sideOverview(a, ed, owner, bs, L);
  }

  // ---- карточка сведений (карточка постройки или плашка требования к общей постройке)
  Id tipId = (hoverNode && !ed.drag && !ed.conn && !ui::anyModalOpen()) ? hoverNode : 0;
  const Ext* tipExt = hoverExt >= 0 && !ui::anyModalOpen() ? &exts[size_t(hoverExt)] : nullptr;
  u64 tipKey = tipExt ? hash64("bt-ext") ^ (u64(tipExt->b) << 32 | tipExt->req) : tipId ? hash64("bt") ^ tipId : 0;
  bool tipShow = hoverDelay(tipKey);
  if (tipShow && tipExt) {
    if (const Building* rb = w.building(tipExt->req)) {
      int lvl = 1;
      if (const BN* b = find(bs, tipExt->b))
        for (const BuildingReq& r : b->reqs)
          if (r.building == tipExt->req) lvl = r.level;
      Tip tip;
      tip.title = rb->name.empty() ? "Без названия" : rb->name;
      const Faction* of = rb->owner ? w.faction(rb->owner) : nullptr;
      tip.subtitle = (of ? "Уникальная · " + of->name : std::string("Общее дерево")) + " · " + bld::catName(rb->cat);
      tip.accent = bld::catColor(rb->cat);
      tip.icon = bld::iconOf(*rb);
      tip.lines.push_back({"link", "Нужна в провинции уровня " + roman(lvl) + " или выше", th.warning, true});
      tip.lines.push_back({"arrow-right", of ? "Щелчок — открыть её дерево" : "Щелчок — открыть в общем дереве", Color(0, 0, 0, 0)});
      tipCard(tip, toScreen(ed.cam, canvas, tipExt->pill.x, tipExt->pill.y, tipExt->pill.w, tipExt->pill.h), canvas);
    }
  }
  if (tipShow && tipId && !tipExt) {
    if (const BN* b = find(bs, tipId)) {
      Tip tip;
      tip.title = bname(*b);
      tip.subtitle = std::string(bld::catName(b->cat)) + " · " + std::to_string(b->levels.size()) + " " + plural(i64(b->levels.size()), "уровень", "уровня", "уровней");
      tip.accent = bld::catColor(b->cat);
      tip.icon = !b->icon.empty() && gfx::hasIcon(b->icon) ? b->icon : bld::catIcon(b->cat);
      if (!trim(b->desc).empty()) tip.lines.push_back({"", b->desc, th.textDim, true});
      for (size_t li = 0; li < b->levels.size() && li < 6; li++) {
        const BuildingLevel& lv = b->levels[li];
        tip.lines.push_back({"slots", roman(int(li) + 1) + " · " + nTurns(std::max(1, lv.turns)) + " · " + bld::costText(w, lv.cost), Color(0, 0, 0, 0), true});
        for (Id mid : lv.modifiers)
          if (const Modifier* m = w.modifier(mid))
            for (int fx = 0; fx < kFxCount; fx++)
              if (m->has(Fx(fx)) && m->fx[size_t(fx)] != 0)
                tip.lines.push_back({schema::effect(Fx(fx)).icon, w::effectText(Fx(fx), m->fx[size_t(fx)]),
                                     w::effectGood(Fx(fx), m->fx[size_t(fx)]) ? th.success : th.danger});
      }
      for (const BuildingReq& r : b->reqs)
        if (const Building* rb = w.building(r.building))
          tip.lines.push_back({"link", "Требует: " + rb->name + (r.level > 1 ? " (ур. " + roman(r.level) + ")" : ""), th.warning});
      if (b->built + b->building > 0)
        tip.lines.push_back({"province", "В провинциях: " + std::to_string(b->built) + (b->building ? ", строится " + std::to_string(b->building) : ""), Color(0, 0, 0, 0)});
      Vec2 p = cardPos(L, *b);
      tipCard(tip, toScreen(ed.cam, canvas, p.x, p.y, kBW, kBH), canvas);
    }
  }
  if (openExt) openBuildingTree(a, openExtOwner, openExt);
}

EditorReg regBuildings({"buildings", "Дерево построек", drawBuildingTree, "building"});

}  // namespace

void openBuildingTree(App& a, Id owner, Id building) {
  pending() = Pending{owner, building, building != 0};
  a.openEditor("buildings", owner);
}

}  // namespace rg::app
