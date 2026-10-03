// Regnum — полноэкранный редактор дерева технологий фракции (ТЗ 1.b.v): схема с панорамой и масштабом,
// карточки технологий (название, бонус, срок изучения, «галочка» изученности, ход исследования), связи-зависимости
// (несколько предшествующих, без циклов), перетаскивание карточек, создание и удаление, авторасстановка,
// копирование дерева другой фракции, панель свойств выбранной технологии.
#include <unordered_map>

#include "app/editors/techtree.h"
#include "app/widgets.h"

namespace rg::app {

namespace {

using namespace tree;
using platform::Key;

constexpr float kW = 232, kH = 96;   // карточка технологии (единицы схемы)
constexpr float kSideW = 356;

// ---------------------------------------------------------------- состояние технологии
enum class St : u8 { Locked, Available, Research, Studied };

const char* stIcon(St s) {
  switch (s) {
    case St::Studied: return "check";
    case St::Research: return "hourglass";
    case St::Available: return "research";
    default: return "lock";
  }
}
const char* stName(St s) {
  switch (s) {
    case St::Studied: return "Изучена";
    case St::Research: return "Исследуется";
    case St::Available: return "Доступна";
    default: return "Закрыта";
  }
}
ui::Tone stTone(St s) {
  switch (s) {
    case St::Studied: return ui::Tone::Success;
    case St::Research: return ui::Tone::Info;
    case St::Available: return ui::Tone::Accent;
    default: return ui::Tone::Neutral;
  }
}
Color stColor(const Ink& k, St s) {
  switch (s) {
    case St::Studied: return k.success;
    case St::Research: return k.info;
    case St::Available: return k.accent;
    default: return k.textMuted;
  }
}

int turnsLeft(int turns, int progress) { return std::max(1, turns - std::max(0, progress)); }

// Копия технологии на кадр (действия посреди кадра заменяют мир — указатели на записи не держим).
struct TN {
  Id id = 0;
  std::string name, desc;
  int turns = 1, progress = 0;
  bool studied = false, research = false;
  Vec2 pos;
  std::vector<Id> prereqs, mods;
  St st = St::Locked;
  std::vector<Id> missing;
  bool match = true;
};

struct LinkSel {
  Id pre = 0, tech = 0;   // tech требует pre
  explicit operator bool() const { return pre && tech; }
  bool operator==(const LinkSel&) const = default;
};

struct Ed {
  Id sel = 0;
  LinkSel link;
  Camera cam;
  Pan pan;
  bool bgDragged = false;
  Id drag = 0;
  Vec2 dragStart;
  Id conn = 0;
  bool connOut = true;
  std::string query;
  Id focusName = 0;         // технология, чьё имя получит фокус клавиатуры
  bool skipClick = false;   // отпускание после двойного щелчка — не снимать выделение
  bool refit = false;       // вписать дерево в следующем кадре (после правки раскладки)
  bool revealSel = false;
  Id menuTech = 0;
  LinkSel menuLink;
  Vec2 menuAt;
  Flag flag;   // копия флага: ui::flag держит ссылку до конца кадра
};

struct Pending {
  Id faction = 0, tech = 0;
  Id refit = 0;   // фракция, чьё дерево вписать в следующем кадре (после копирования из подтверждения)
};
Pending& pending() {
  static Pending p;
  return p;
}

double snap(double v) { return std::round(v / 4) * 4; }

Box2 boundsOf(const std::vector<TN>& ts) {
  Box2 b;
  for (const TN& t : ts) {
    b.add(t.pos);
    b.add({t.pos.x + kW, t.pos.y + kH});
  }
  return b;
}

const TN* find(const std::vector<TN>& ts, Id id) {
  for (const TN& t : ts)
    if (t.id == id) return &t;
  return nullptr;
}

std::string techNames(const std::vector<TN>& ts, const std::vector<Id>& ids, size_t maxN = 3) {
  std::vector<std::string> n;
  for (Id id : ids) {
    if (n.size() >= maxN) {
      n.push_back("ещё " + std::to_string(ids.size() - maxN));
      break;
    }
    if (const TN* t = find(ts, id)) n.push_back(t->name.empty() ? std::string("Без названия") : t->name);
  }
  return join(n, ", ");
}

// Краткий бонус: описание или эффекты модификаторов.
std::string bonusText(const World& w, const TN& t) {
  if (!trim(t.desc).empty()) return t.desc;
  std::vector<std::string> parts;
  for (Id mid : t.mods)
    if (const Modifier* m = w.modifier(mid))
      for (int f = 0; f < kFxCount; f++)
        if (m->has(Fx(f)) && m->fx[size_t(f)] != 0) parts.push_back(w::effectText(Fx(f), m->fx[size_t(f)]));
  return join(parts, "; ");
}

// ---------------------------------------------------------------- сцена (рисуется при сведении слоя)
struct Card {
  TN t;
  std::string bonus, need;
  bool sel = false, hover = false, dim = false, drop = false, dropBad = false;
  bool checkHot = false, outHot = false, inHot = false, hasIn = false, hasOut = false;
};
struct EdgeD {
  Curve k;
  Color col;
  float width = 2;
  bool hot = false, sel = false;
};
struct Scene {
  Camera cam;
  Ink k;
  std::vector<Card> cards;
  std::vector<EdgeD> edges;
  bool pending = false;
  Curve pend;
  Color pendCol;
  bool readOnly = false;
};

void drawArc(gfx::Canvas& c, float cx, float cy, float r, float frac, float width, Color col) {
  if (frac <= 0) return;
  gfx::Path p;
  int n = std::max(8, int(48 * frac));
  for (int i = 0; i <= n; i++) {
    float a = float(-kPi / 2 + 2 * kPi * double(frac) * double(i) / double(n));
    float x = cx + r * std::cos(a), y = cy + r * std::sin(a);
    if (i == 0) p.moveTo(x, y);
    else p.lineTo(x, y);
  }
  gfx::Stroke st;
  st.width = width;
  st.cap = gfx::Cap::Round;
  st.join = gfx::Join::Round;
  c.strokePath(p, st, gfx::Paint(col));
}

void drawCard(gfx::Canvas& c, const Scene& s, const Card& cd) {
  const Ink& k = s.k;
  const TN& t = cd.t;
  const float z = float(s.cam.z);
  const float px = 1 / z;   // одна точка интерфейса в единицах схемы
  RectF r{float(t.pos.x), float(t.pos.y), kW, kH};
  Color tone = stColor(k, t.st);
  c.save();
  if (cd.dim) c.setOpacity(0.28f);
  // Тень и подложка
  c.boxShadow(r, 12, cd.sel || cd.hover ? 26 : 14, 0, k.shadow.alpha(cd.sel || cd.hover ? 0.95f : 0.7f), gfx::Pt{0, 5});
  float tint = t.st == St::Locked ? 0.0f : (k.dark ? 0.11f : 0.08f);
  gfx::Gradient g;
  g.kind = gfx::Gradient::Linear;
  g.p0 = {r.x, r.y};
  g.p1 = {r.x, r.bottom()};
  g.stops = {{0.f, Color::mix(k.surface, tone, tint)}, {1.f, k.surface}};
  gfx::Paint gp;
  gp.gradient = &g;
  c.fillRoundRect(r, 12, gp);
  // Полоска состояния сверху
  c.save();
  c.clipRoundRect(r, 12);
  c.fillRect(RectF{r.x, r.y, r.w, 3}, tone.alpha(t.st == St::Locked ? 0.35f : 0.9f));
  c.restore();
  Color bc = cd.sel ? k.accent : cd.hover ? Color::mix(k.borderStrong, tone, 0.45f) : Color::mix(k.border, tone, t.st == St::Locked ? 0.f : 0.3f);
  c.strokeRoundRect(r.inset(0.5f * px), 12, (cd.sel ? 1.6f : 1.f) * px, bc);
  if (cd.sel) c.strokeRoundRect(r.expand(4 * px), 12 + 4 * px, 2 * px, k.accent.alpha(0.45f));
  if (cd.drop) c.strokeRoundRect(r.expand(4 * px), 12 + 4 * px, 2.2f * px, (cd.dropBad ? k.danger : k.success).alpha(0.9f));

  // Медальон состояния (у исследуемой — кольцо хода)
  float mx = r.x + 28, my = r.y + 30;
  c.fillCircle(mx, my, 17, tone.alpha(t.st == St::Locked ? 0.12f : 0.17f));
  if (t.st == St::Research) {
    c.strokeCircle(mx, my, 17, 2.4f, k.surfaceHi);
    drawArc(c, mx, my, 17, clamp(float(t.progress) / float(std::max(1, t.turns)), 0.f, 1.f), 2.4f, tone);
  }
  icon(c, stIcon(t.st), RectF{mx - 9, my - 9, 18, 18}, tone);

  // Название, срок, модификаторы
  Color nameCol = t.st == St::Locked ? k.textDim : k.text;
  text(c, t.name.empty() ? "Без названия" : t.name, textStyle(14, gfx::FontWeight::Semibold), RectF{r.x + 54, r.y + 11, kW - 54 - 40, 20}, nameCol);
  {
    float x = r.x + 54, y = r.y + 33;
    icon(c, "hourglass", RectF{x, y + 1, 13, 13}, k.textMuted);
    std::string tt = t.st == St::Research ? "ещё " + nTurns(turnsLeft(t.turns, t.progress)) : nTurns(t.turns);
    gfx::TextStyle st = textStyle(11.5f);
    float tw = gfx::measureText(tt, st);
    text(c, tt, st, RectF{x + 17, y, tw + 2, 15}, k.textMuted);
    x += 17 + tw + 10;
    if (!t.mods.empty()) {
      icon(c, "sparkles", RectF{x, y + 1, 13, 13}, k.accent.alpha(0.9f));
      std::string mt = std::to_string(t.mods.size());
      text(c, mt, st, RectF{x + 16, y, 24, 15}, k.textMuted);
    }
  }
  // «Галочка» изученности (ТЗ 1.b.v)
  {
    RectF cb{r.right() - 31, r.y + 13, 18, 18};
    if (t.studied) {
      c.fillRoundRect(cb, 5, k.success);
      icon(c, "check", cb.inset(2), k.success.textOn());
    } else {
      if (cd.checkHot && !s.readOnly) c.fillRoundRect(cb, 5, k.accent.alpha(0.18f));
      c.strokeRoundRect(cb.inset(0.6f * px), 5, 1.4f * px, cd.checkHot && !s.readOnly ? k.accent : k.borderStrong);
    }
  }
  // Бонус и ход исследования
  gfx::TextStyle small = textStyle(11.5f);
  if (t.st == St::Research || (t.progress > 0 && !t.studied)) {
    std::string b = cd.bonus.empty() ? std::string("Без бонуса") : cd.bonus;
    text(c, b, small, RectF{r.x + 14, r.y + 56, kW - 28, 16}, cd.bonus.empty() ? k.textMuted : k.textDim);
    RectF bar{r.x + 14, r.y + 79, kW - 28 - 34, 5};
    c.fillRoundRect(bar, 2.5f, k.surfaceHi);
    float fr = clamp(float(t.progress) / float(std::max(1, t.turns)), 0.f, 1.f);
    if (fr > 0) c.fillRoundRect(RectF{bar.x, bar.y, std::max(5.f, bar.w * fr), bar.h}, 2.5f, t.research ? k.info : k.textMuted);
    text(c, std::to_string(t.progress) + "/" + std::to_string(t.turns), small, RectF{bar.right() + 4, bar.y - 6, 30, 16}, k.textMuted, gfx::Align::Right);
  } else if (!cd.bonus.empty()) {
    text(c, cd.bonus, small, RectF{r.x + 14, r.y + 54, kW - 28, 34}, k.textDim, gfx::Align::Left, 2, gfx::VAlign::Top);
  } else if (!cd.need.empty()) {
    text(c, cd.need, small, RectF{r.x + 14, r.y + 54, kW - 28, 34}, k.textMuted, gfx::Align::Left, 2, gfx::VAlign::Top);
  } else {
    text(c, "Без бонуса", small, RectF{r.x + 14, r.y + 56, kW - 28, 16}, k.textMuted);
  }
  // Гнёзда связей
  Vec2 in{r.x, r.y + kH * 0.5}, out{r.right(), r.y + kH * 0.5};
  if (cd.hasIn || cd.inHot || cd.hover || cd.sel) drawPort(c, in, cd.inHot ? k.accent : Color::mix(k.borderStrong, tone, 0.5f), k.bg, cd.hasIn, cd.inHot, z);
  if (cd.hasOut || cd.outHot || cd.hover || cd.sel) drawPort(c, out, cd.outHot ? k.accent : Color::mix(k.borderStrong, tone, 0.5f), k.bg, cd.hasOut, cd.outHot, z);
  c.restore();
}

void renderScene(const Scene& s, gfx::Canvas& c, RectF dev, float scale) {
  c.save();
  c.clipRoundRect(dev, 10 * scale);
  drawGrid(c, dev, scale, s.cam, s.k);
  applyCamera(c, dev, scale, s.cam);
  const float z = float(s.cam.z);
  for (const EdgeD& e : s.edges) {
    if (e.sel) drawCurve(c, e.k, s.k.accent.alpha(0.22f), e.width + 7, z, false);
    drawCurve(c, e.k, e.col, e.width, z, true);
  }
  for (const Card& cd : s.cards) drawCard(c, s, cd);
  if (s.pending) drawCurve(c, s.pend, s.pendCol, 2.2f, z, true, true);
  c.restore();
}

// ---------------------------------------------------------------- действия
void askDelete(App& a, Id tech, const std::string& name) {
  a.confirm("Удалить технологию «" + (name.empty() ? std::string("Без названия") : name) + "»?",
            "Связи с ней исчезнут, зависящие технологии перестанут её требовать. Действие можно отменить Ctrl+Z.", "Удалить", true,
            [tech](App& x) { x.act("Удалить технологию", [&](Tx& tx) { rules::removeTech(tx, tech); }); });
}

void removeLink(App& a, LinkSel l) {
  a.act("Удалить связь технологий", [&](Tx& tx) { rules::setPrereq(tx, l.tech, l.pre, false); });
}

Id createAt(App& a, Id faction, Vec2 center) {
  Id nid = 0;
  Vec2 p{snap(center.x - kW * 0.5), snap(center.y - kH * 0.5)};
  if (!a.act("Новая технология", [&](Tx& tx) {
        nid = rules::createTech(tx, faction, "Новая технология");
        tx.tech(nid).pos = p;
      }))
    return 0;
  return nid;
}

// Копия дерева другой фракции (ТЗ 1.b.v: деревья редактируются и дополняются). Копии ставятся ниже дерева.
void copyTree(App& a, Id from, Id to) {
  const World& w = a.world();
  const Faction* f = w.faction(from);
  if (!f) return;
  int n = 0;
  w.techs.each([&](const Tech& t) { n += t.faction == from; });
  std::string fname = f->name;
  if (a.act("Скопировать дерево технологий", [&](Tx& tx) { rules::copyTechTree(tx, from, to); })) {
    a.toast("Добавлено " + std::to_string(n) + " " + plural(n, "технология", "технологии", "технологий") + " из дерева «" + fname + "»",
            ToastKind::Success, "copy");
    pending().refit = to;
  }
}

void copyMenu(App& a, Id faction, bool nonEmpty) {
  if (!ui::beginMenu("copyfrom")) return;
  const World& w = a.world();
  const bool ro = a.readOnly();
  ui::menuHeader("Добавить копию дерева");
  std::vector<const Faction*> fs;
  w.factions.each([&](const Faction& f) {
    if (f.id != faction) fs.push_back(&f);
  });
  std::sort(fs.begin(), fs.end(), [](const Faction* x, const Faction* y) {
    if (x->kind != y->kind) return x->kind < y->kind;
    return compareRu(x->name, y->name) < 0;
  });
  for (const Faction* f : fs) {
    int n = 0;
    w.techs.each([&](const Tech& t) { n += t.faction == f->id; });
    ui::IdScope s{i64(f->id)};
    std::string label = (f->name.empty() ? std::string("Без названия") : f->name) + " · " + std::to_string(n);
    if (ui::menuItem(label, {.icon = f->isGuild() ? "guild" : "crown", .disabled = n == 0 || ro})) {
      Id from = f->id;
      if (!nonEmpty) {
        copyTree(a, from, faction);
      } else {
        // В дереве уже есть технологии — копии добавятся к ним: спросить.
        a.confirm("Добавить копию дерева «" + f->name + "»?",
                  std::to_string(n) + " " + plural(n, "технология", "технологии", "технологий") +
                      " появятся ниже текущего дерева — неизученными, со своими связями. Действие можно отменить Ctrl+Z.",
                  "Добавить", false, [from, faction](App& x) { copyTree(x, from, faction); });
      }
    }
    a.markUi("tt.copy." + std::to_string(f->id));
  }
  if (fs.empty()) ui::menuItem("Других фракций нет", {.disabled = true});
  ui::endMenu();
}

// Ближайшая технология в направлении (стрелки клавиатуры).
Id neighbor(const std::vector<TN>& ts, const TN& cur, int dx, int dy) {
  // Сначала — по связям.
  if (dx < 0 && !cur.prereqs.empty()) {
    Id best = 0;
    double bd = kInf;
    for (Id p : cur.prereqs)
      if (const TN* t = find(ts, p); t && std::fabs(t->pos.y - cur.pos.y) < bd) {
        bd = std::fabs(t->pos.y - cur.pos.y);
        best = p;
      }
    if (best) return best;
  }
  if (dx > 0) {
    Id best = 0;
    double bd = kInf;
    for (const TN& t : ts)
      if (std::find(t.prereqs.begin(), t.prereqs.end(), cur.id) != t.prereqs.end() && std::fabs(t.pos.y - cur.pos.y) < bd) {
        bd = std::fabs(t.pos.y - cur.pos.y);
        best = t.id;
      }
    if (best) return best;
  }
  Id best = 0;
  double bs = kInf;
  for (const TN& t : ts) {
    if (t.id == cur.id) continue;
    double ddx = t.pos.x - cur.pos.x, ddy = t.pos.y - cur.pos.y;
    double along = dx ? ddx * dx : ddy * dy;
    double across = dx ? std::fabs(ddy) : std::fabs(ddx);
    if (along <= 1) continue;
    double score = along + across * 2.5;
    if (score < bs) {
      bs = score;
      best = t.id;
    }
  }
  return best;
}

// ---------------------------------------------------------------- панель свойств
void sideOverview(App& a, Ed& ed, Id faction, const std::vector<TN>& ts) {
  const World& w = a.world();
  const Faction* f = w.faction(faction);
  int studied = 0, research = 0, avail = 0;
  for (const TN& t : ts) {
    studied += t.studied;
    research += t.st == St::Research;
    avail += t.st == St::Available;
  }
  {
    ui::Row r({ui::px(54), ui::fr(1)}, 40, 12);
    {
      RectF fr = ui::next(54, 40);
      ui::at(RectF{fr.x, fr.y + 2, 54, 36});
      ui::flag(ed.flag, 54, 36, 5);
    }
    ui::Group g(0, 0);
    ui::caption(f && f->isGuild() ? "Торговая гильдия" : "Государство");
    ui::label(f ? f->name : std::string("—"), {.font = ui::Font::Subtitle});
  }
  ui::spacer(4);
  {
    ui::Row r({ui::fr(1), ui::fr(1)}, 64, 10);
    ui::stat(std::to_string(studied) + " / " + std::to_string(ts.size()), "Изучено", {.icon = "check-circle", .tone = ui::Tone::Success});
    ui::stat(std::to_string(research), "Исследуется", {.icon = "hourglass", .tone = ui::Tone::Info});
  }
  if (!ts.empty()) ui::progress(double(studied) / double(ts.size()), {.tone = ui::Tone::Success, .height = 5, .label = true});
  if (research > 0) {
    if (ui::Section s("Исследуются", "hourglass", {.badge = std::to_string(research)}); s) {
      for (const TN& t : ts) {
        if (t.st != St::Research) continue;
        ui::IdScope sc{i64(t.id)};
        if (ui::listItem(t.name, {.icon = "research", .subtitle = "Осталось " + nTurns(turnsLeft(t.turns, t.progress)),
                                  .hint = std::to_string(t.progress) + "/" + std::to_string(t.turns)})) {
          ed.sel = t.id;
          ed.revealSel = true;
        }
        ui::progress(double(t.progress) / double(std::max(1, t.turns)), {.tone = ui::Tone::Info, .height = 4});
      }
    }
  }
  if (avail > 0) {
    if (ui::Section s("Можно исследовать", "research", {.badge = std::to_string(avail)}); s) {
      for (const TN& t : ts) {
        if (t.st != St::Available) continue;
        ui::IdScope sc{i64(t.id)};
        if (ui::listItem(t.name, {.icon = "research", .hint = nTurns(t.turns)})) {
          ed.sel = t.id;
          ed.revealSel = true;
        }
      }
    }
  }
  if (ui::Section s("Обозначения", "info", {.defaultOpen = ts.size() < 3}); s) {
    for (St st : {St::Studied, St::Research, St::Available, St::Locked}) {
      ui::IdScope sc{int(st)};
      ui::Row r({ui::px(18), ui::fr(1)}, 22, 8);
      ui::iconColored(stIcon(st), ui::toneColor(stTone(st)), 16);
      ui::label(stName(st), {.ink = ui::Ink::Dim});
    }
    ui::separator();
    ui::label("Двойной щелчок по фону — новая технология", {.font = ui::Font::Small, .ink = ui::Ink::Muted, .icon = "plus", .wrap = true});
    ui::label("Тяните от гнезда карточки к другой — зависимость", {.font = ui::Font::Small, .ink = ui::Ink::Muted, .icon = "link", .wrap = true});
    ui::label("Щелчок по связи — выбрать, Delete — удалить", {.font = ui::Font::Small, .ink = ui::Ink::Muted, .icon = "unlink", .wrap = true});
    ui::label("Колесо — масштаб, фон — перетаскивание, F — всё дерево", {.font = ui::Font::Small, .ink = ui::Ink::Muted, .icon = "zoom-fit", .wrap = true});
  }
}

void sideLink(App& a, Ed& ed, const std::vector<TN>& ts) {
  const TN* pre = find(ts, ed.link.pre);
  const TN* tech = find(ts, ed.link.tech);
  if (!pre || !tech) return;
  ui::caption("Зависимость");
  {
    // Требуемая → зависимая, по строке на каждую (названия бывают длинными).
    ui::Group g(0, 4);
    if (ui::chip(pre->name + "##pre", {.icon = stIcon(pre->st), .tone = stTone(pre->st), .clickable = true, .tooltip = "Выбрать"}) == ui::ChipAction::Click) {
      ed.sel = pre->id;
      ed.link = {};
      ed.revealSel = true;
    }
    {
      ui::Indent in(10);
      ui::icon("arrow-down", ui::Ink::Muted, 16);
    }
    if (ui::chip(tech->name + "##tech", {.icon = stIcon(tech->st), .tone = stTone(tech->st), .clickable = true, .tooltip = "Выбрать"}) == ui::ChipAction::Click) {
      ed.sel = tech->id;
      ed.link = {};
      ed.revealSel = true;
    }
  }
  ui::text("Для изучения «" + tech->name + "» нужна «" + pre->name + "».", ui::Font::Small, ui::Ink::Dim);
  ui::spacer(4);
  ui::Disabled d(a.readOnly());
  if (ui::button("Удалить связь", {.variant = ui::Variant::Danger, .icon = "unlink", .fill = true, .tooltip = "Удалить зависимость (Delete)"})) {
    removeLink(a, ed.link);
    ed.link = {};
  }
  a.markUi("tt.side.unlink");
}

void sideTech(App& a, Ed& ed, const std::vector<TN>& ts, const TN& t) {
  const World& w = a.world();
  const bool ro = a.readOnly();
  const Color none(0, 0, 0, 0);
  ui::IdScope scope{i64(t.id)};
  {
    ui::HStack hs(28, ui::Align::Left, 6);
    ui::tag(stName(t.st), stTone(t.st), stIcon(t.st));
    ui::flex();
    if (ui::iconButton("target", "Показать на схеме")) ed.revealSel = true;
    if (ui::iconButton("trash", "Удалить технологию", {.disabled = ro, .shortcut = {Key::Delete, 0}, .tone = ui::Tone::Danger})) askDelete(a, t.id, t.name);
    a.markUi("tt.side.delete");
  }
  {
    ui::Disabled dis(ro);
    // Название
    if (ed.focusName == t.id) {
      ui::setKeyboardFocus(ui::id("name"));
      ed.focusName = 0;
    }
    std::string name = t.name;
    if (ui::textField("name", name, {.placeholder = "Название технологии", .maxLength = 80, .selectAllOnFocus = true}) && !trim(name).empty() &&
        trim(name) != t.name) {
      std::string n = trim(name);
      a.act("Переименовать технологию", [&](Tx& tx) { tx.tech(t.id).name = n; });
    }
    a.markUi("tt.side.name");
    // «Галочка» изученности (ТЗ 1.b.v)
    bool studied = t.studied;
    if (ui::checkbox("Изучена", studied)) a.act(studied ? "Отметить изученной" : "Снять отметку изучения", [&](Tx& tx) { rules::setStudied(tx, t.id, studied); });
    a.markUi("tt.side.studied");
  }
  // Исследование
  {
    ui::Card card({.pad = 12, .tone = stTone(t.st)});
    if (t.studied) {
      ui::label("Технология изучена — её бонусы действуют.", {.ink = ui::Ink::Success, .icon = "check-circle", .wrap = true});
    } else if (t.research) {
      ui::Row r({ui::fr(1), ui::px(60)}, 22, 8);
      ui::label("Исследуется · ещё " + nTurns(turnsLeft(t.turns, t.progress)), {.font = ui::Font::Strong, .ink = ui::Ink::Info});
      ui::label(std::to_string(t.progress) + " / " + std::to_string(t.turns), {.ink = ui::Ink::Dim, .align = ui::Align::Right});
    } else if (t.st == St::Available) {
      ui::label("Условия выполнены. Изучение займёт " + nTurns(turnsLeft(t.turns, t.progress)) + ".", {.ink = ui::Ink::Dim, .wrap = true});
    } else {
      ui::label("Сначала изучите:", {.ink = ui::Ink::Dim, .icon = "lock"});
      ui::IdScope ms("missing");
      tree::ChipFlow flow;
      for (Id m : t.missing) {
        const TN* mt = find(ts, m);
        if (!mt) continue;
        ui::IdScope s2{i64(m)};
        if (tree::chip(mt->name, {.icon = stIcon(mt->st), .tone = stTone(mt->st), .clickable = true, .tooltip = "Выбрать"}) == ui::ChipAction::Click) {
          ed.sel = m;
          ed.revealSel = true;
        }
      }
    }
    if (t.research) {
      ui::progress(double(t.progress) / double(std::max(1, t.turns)), {.tone = ui::Tone::Info, .height = 6});
      if (ui::button("Остановить исследование", {.icon = "close", .fill = true, .disabled = ro}))
        a.act("Остановить исследование", [&](Tx& tx) { rules::stopResearch(tx, t.id); });
      a.markUi("tt.side.research");
    } else if (!t.studied) {
      if (t.progress > 0) ui::progress(double(t.progress) / double(std::max(1, t.turns)), {.tone = ui::Tone::Neutral, .height = 4});
      bool can = t.st == St::Available;
      if (ui::button("Начать исследование", {.variant = ui::Variant::Primary, .icon = "play", .fill = true, .disabled = ro || !can,
                                              .tooltip = can ? std::string_view("Исследование продвигается на один ход при завершении хода")
                                                             : std::string_view("Не изучены предшествующие технологии")}))
        a.act("Начать исследование", [&](Tx& tx) { rules::startResearch(tx, t.id); });
      a.markUi("tt.side.research");
    }
  }
  {
    ui::Disabled dis(ro);
    // Срок изучения
    ui::prop("Срок изучения", "hourglass");
    int turns = t.turns;
    if (ui::numberField("turns", turns, {.min = 1, .max = 999, .unit = "ход|хода|ходов", .steppers = true}))
      a.act("Срок изучения технологии", [&](Tx& tx) { tx.tech(t.id).turns = std::max(1, turns); }, {.coalesce = "tt-turns:" + std::to_string(t.id)});
    a.markUi("tt.side.turns");
    // Бонус (описание)
    const Tech* rec = w.tech(t.id);
    const Faction* owner = rec ? w.faction(rec->faction) : nullptr;
    ui::caption(owner && owner->isGuild() ? "Бонус для гильдии" : "Бонус для государства");
    std::string desc = t.desc;
    if (ui::textArea("desc", desc, 76, {.placeholder = "Что даёт технология: «+10 % к торговле», «осадные орудия»…", .maxLength = 600}) && desc != t.desc)
      a.act("Описание технологии", [&](Tx& tx) { tx.tech(t.id).desc = desc; });
    a.markUi("tt.side.desc");
  }
  // Модификаторы и их эффекты (бонус технологии в расчётах)
  ui::caption("Модификаторы");
  std::vector<Id> mods = t.mods;
  if (tree::modifierList("mods", mods, ro)) a.act("Модификаторы технологии", [&](Tx& tx) { tx.tech(t.id).modifiers = mods; });
  a.markUi("tt.side.mods");
  {
    ui::IdScope fs("fx");
    tree::effectChips(w, t.mods);
  }
  // Зависимости
  if (ui::Section s("Требует", "link", {.badge = t.prereqs.empty() ? std::string() : std::to_string(t.prereqs.size())}); s) {
    if (!t.prereqs.empty()) {
      ui::IdScope ps("pre");
      tree::ChipFlow flow;
      for (Id p : t.prereqs) {
        const TN* pt = find(ts, p);
        if (!pt) continue;
        ui::IdScope s2{i64(p)};
        ui::ChipAction ca = tree::chip(pt->name, {.icon = stIcon(pt->st), .tone = stTone(pt->st), .removable = !ro, .clickable = true, .tooltip = "Выбрать"});
        if (ca == ui::ChipAction::Click) {
          ed.sel = p;
          ed.revealSel = true;
        } else if (ca == ui::ChipAction::Remove) {
          removeLink(a, {p, t.id});
        }
      }
    } else {
      ui::label("Нет условий — можно изучать сразу.", {.font = ui::Font::Small, .ink = ui::Ink::Muted});
    }
    // Добавить предшествующую: варианты, создающие цикл, недоступны.
    std::vector<std::string> labels;
    std::vector<Id> ids;
    std::vector<char> bad;
    for (const TN& o : ts) {
      if (o.id == t.id || std::find(t.prereqs.begin(), t.prereqs.end(), o.id) != t.prereqs.end()) continue;
      ids.push_back(o.id);
      labels.push_back(o.name.empty() ? std::string("Без названия") : o.name);
      bad.push_back(rules::wouldCycle(w, t.id, o.id) ? 1 : 0);
    }
    if (!ids.empty()) {
      std::vector<ui::Option> opts;
      for (size_t i = 0; i < ids.size(); i++) {
        const TN* o = find(ts, ids[i]);
        opts.push_back(ui::Option{labels[i], o ? stIcon(o->st) : "tech", none, bad[i] ? std::string_view("цикл") : std::string_view(), bad[i] != 0});
      }
      int idx = -1;
      if (ui::combo("addpre", idx, std::span<const ui::Option>(opts), {.placeholder = "Добавить условие", .icon = "plus", .disabled = ro}) && idx >= 0) {
        Id pre = ids[size_t(idx)];
        a.act("Связать технологии", [&](Tx& tx) { rules::setPrereq(tx, t.id, pre, true); });
      }
      a.markUi("tt.side.addpre");
    }
  }
  std::vector<Id> deps;
  for (const TN& o : ts)
    if (std::find(o.prereqs.begin(), o.prereqs.end(), t.id) != o.prereqs.end()) deps.push_back(o.id);
  if (!deps.empty()) {
    if (ui::Section s("Открывает", "arrow-right", {.badge = std::to_string(deps.size())}); s) {
      ui::IdScope ds("deps");
      tree::ChipFlow flow;
      for (Id d : deps) {
        const TN* dt = find(ts, d);
        ui::IdScope s2{i64(d)};
        if (tree::chip(dt->name, {.icon = stIcon(dt->st), .tone = stTone(dt->st), .clickable = true, .tooltip = "Выбрать"}) == ui::ChipAction::Click) {
          ed.sel = d;
          ed.revealSel = true;
        }
      }
    }
  }
}

// ---------------------------------------------------------------- редактор
void drawTechTree(App& a, Id faction) {
  const World& w = a.world();
  const ui::Theme& th = ui::theme();
  const bool ro = a.readOnly();
  RectF R = ui::avail();
  const Faction* fac = w.faction(faction);
  if (!fac) {
    // Фракции нет (удалена или не выбрана) — предложить выбрать.
    ui::Area ar(RectF{R.cx() - 180, R.cy() - 120, 360, 240}, 0);
    ui::emptyState("tech-tree", "Выберите государство или гильдию.");
    Id pick = 0;
    if (factionSwitch("pick", pick, false)) a.openEditor("techtree", pick);
    return;
  }
  Ed& ed = ui::state<Ed>(ui::id("tt#" + std::to_string(faction)));
  ed.flag = fac->flag;
  const std::string facName = fac->name;
  const bool guild = fac->isGuild();

  // Снимок дерева на кадр.
  std::vector<TN> ts;
  w.techs.each([&](const Tech& t) {
    if (t.faction != faction) return;
    TN n;
    n.id = t.id;
    n.name = t.name;
    n.desc = t.desc;
    n.turns = std::max(1, t.turns);
    n.progress = t.progress;
    n.studied = t.studied;
    n.research = t.research;
    n.pos = t.pos;
    n.prereqs = t.prereqs;
    n.mods = t.modifiers;
    ts.push_back(std::move(n));
  });
  for (TN& t : ts) {
    if (t.studied) t.st = St::Studied;
    else {
      rules::ResearchCheck rc = rules::canResearch(w, t.id);
      t.missing = rc.missing;
      t.st = t.research ? St::Research : (rc.ok ? St::Available : St::Locked);
    }
    t.match = ed.query.empty() || utf8::matches(t.name, ed.query) || utf8::matches(t.desc, ed.query);
  }
  if (Pending& p = pending(); p.faction == faction && p.tech) {
    if (find(ts, p.tech)) {
      ed.sel = p.tech;
      ed.link = {};
      ed.revealSel = true;
    }
    p.faction = p.tech = 0;
  }
  if (ed.sel && !find(ts, ed.sel)) ed.sel = 0;
  if (ed.link) {
    const TN* lt = find(ts, ed.link.tech);
    if (!lt || !find(ts, ed.link.pre) || std::find(lt->prereqs.begin(), lt->prereqs.end(), ed.link.pre) == lt->prereqs.end()) ed.link = {};
  }
  if (ed.focusName && ed.focusName != ed.sel) ed.focusName = 0;
  if (ed.drag && !find(ts, ed.drag)) ed.drag = 0;
  if (ed.conn && !find(ts, ed.conn)) ed.conn = 0;
  int studiedN = 0, researchN = 0;
  for (const TN& t : ts) {
    studiedN += t.studied;
    researchN += t.st == St::Research;
  }

  // Раскладка: панель инструментов, холст, панель свойств.
  RectF bar = R.cutTop(36);
  R.cutTop(12);
  RectF side = R.cutRight(kSideW);
  R.cutRight(12);
  RectF canvas = R;
  Box2 bounds = boundsOf(ts);
  if (pending().refit == faction) {
    ed.refit = true;
    pending().refit = 0;
  }
  if (!ed.cam.ready && !canvas.empty()) fit(ed.cam, canvas, bounds, false, 1.0);
  else if (ed.refit && !canvas.empty()) fit(ed.cam, canvas, bounds, true, 1.0);
  ed.refit = false;

  // ---- панель инструментов
  {
    ui::Area ar(bar, 0);
    ui::HStack hs(34, ui::Align::Left, 8);
    {
      RectF fr = ui::next(42, 34);
      ui::at(RectF{fr.x, fr.y + 3, 42, 28});
      ui::flag(ed.flag, 42, 28, 4, facName);
    }
    {
      ui::Group g(280, 0);
      ui::spacer(2);
      Id pick = faction;
      if (factionSwitch("faction", pick, false) && pick && pick != faction) a.openEditor("techtree", pick);
      a.markUi("tt.picker");
    }
    ui::tag(std::to_string(studiedN) + " / " + std::to_string(ts.size()) + " изучено", ui::Tone::Success, "check-circle");
    if (researchN) ui::tag(std::to_string(researchN) + " " + plural(researchN, "исследуется", "исследуются", "исследуются"), ui::Tone::Info, "hourglass");
    if (ro) ui::tag("Ход " + std::to_string(a.ui.viewTurn.value_or(0)) + " · только просмотр", ui::Tone::Warning, "lock");
    ui::flex();
    {
      RectF sr = ui::next(220, 34);
      ui::at(RectF{sr.x, sr.y + 2, sr.w, 30});
      std::string q = ed.query;
      if (ui::searchField("search", q, "Найти технологию")) {
        ed.query = q;
        for (const TN& t : ts)
          if (!q.empty() && (utf8::matches(t.name, q) || utf8::matches(t.desc, q))) {
            ed.sel = t.id;
            ed.link = {};
            ed.revealSel = true;
            break;
          }
      }
      a.markUi("tt.search");
    }
    ui::separatorV();
    ui::Disabled d(ro);
    if (ui::iconButton("plus", "Новая технология", {.shortcut = {Key::N, 0}})) {
      Vec2 c = toWorld(ed.cam, canvas, canvas.cx(), canvas.cy());
      if (Id nid = createAt(a, faction, c)) {
        ed.sel = nid;
        ed.link = {};
        ed.focusName = ed.sel;
      }
    }
    a.markUi("tt.add");
    if (ui::iconButton("wand", "Расставить дерево автоматически", {.disabled = ts.size() < 2})) {
      if (a.act("Расставить дерево технологий", [&](Tx& tx) { rules::autoLayout(tx, faction); })) ed.refit = true;
    }
    a.markUi("tt.layout");
    if (ui::iconButton("copy", "Скопировать дерево другой фракции")) ui::openPopup("copyfrom");
    a.markUi("tt.copy");
    ui::separatorV();
    bool canDel = ed.sel || ed.link;
    if (ui::iconButton("trash", ed.link ? "Удалить связь" : "Удалить технологию", {.disabled = !canDel, .shortcut = {Key::Delete, 0}, .tone = ui::Tone::Danger})) {
      if (ed.link) {
        removeLink(a, ed.link);
        ed.link = {};
      } else if (const TN* t = find(ts, ed.sel)) {
        askDelete(a, t->id, t->name);
      }
    }
    a.markUi("tt.delete");
  }

  // ---- холст: ввод
  a.markUi("tt.canvas", canvas);
  const ui::Mouse& mouse = ui::mouse();
  ui::Interaction bg = ui::interact(ui::id("bg"), canvas, ui::IfAllowOverlap | ui::IfMiddleButton | ui::IfRightButton);
  if (bg.pressed) ed.bgDragged = false;
  if (bg.dragging) ed.bgDragged = true;
  Vec2 mw = toWorld(ed.cam, canvas, mouse.x, mouse.y);

  Id hoverNode = 0, hoverCheck = 0, hoverOut = 0, hoverIn = 0;
  bool connReleased = false, openMenu = false;
  // Выбранная карточка — последней (сверху).
  std::vector<size_t> order(ts.size());
  for (size_t i = 0; i < ts.size(); i++) order[i] = i;
  std::stable_partition(order.begin(), order.end(), [&](size_t i) { return ts[i].id != ed.sel && ts[i].id != ed.drag; });
  for (size_t oi : order) {
    const TN& t = ts[oi];
    RectF sr = toScreen(ed.cam, canvas, t.pos.x, t.pos.y, kW, kH);
    bool off = sr.right() < canvas.x - 20 || sr.x > canvas.right() + 20 || sr.bottom() < canvas.y - 20 || sr.y > canvas.bottom() + 20;
    if (off && t.id != ed.drag && t.id != ed.conn) continue;
    ui::IdScope s{i64(t.id)};
    a.markUi("tt.node." + std::to_string(t.id), sr);
    ui::Interaction ni = ui::interact(ui::id("node"), sr.intersect(canvas), ui::IfAllowOverlap | ui::IfRightButton);
    float ps = std::max(18.f, float(16 * ed.cam.z));
    RectF po{sr.right() - ps * 0.5f, sr.cy() - ps * 0.5f, ps, ps}, pin{sr.x - ps * 0.5f, sr.cy() - ps * 0.5f, ps, ps};
    float cs = std::max(18.f, float(22 * ed.cam.z));
    RectF cr{float(sr.right() - 22 * ed.cam.z - cs * 0.5f), float(sr.y + 22 * ed.cam.z - cs * 0.5f), cs, cs};
    ui::Interaction ci = ui::interact(ui::id("check"), cr.intersect(canvas), ui::IfAllowOverlap);
    ui::Interaction oi2 = ui::interact(ui::id("out"), po.intersect(canvas), ui::IfAllowOverlap);
    ui::Interaction ii = ui::interact(ui::id("in"), pin.intersect(canvas), ui::IfAllowOverlap);
    a.markUi("tt.check." + std::to_string(t.id), cr);
    a.markUi("tt.out." + std::to_string(t.id), po);
    a.markUi("tt.in." + std::to_string(t.id), pin);
    if (ni.hovered) hoverNode = t.id;
    if (ci.hovered) hoverCheck = t.id;
    if (oi2.hovered) hoverOut = t.id;
    if (ii.hovered) hoverIn = t.id;
    // Выбор и перетаскивание карточки
    if (ni.pressed) {
      ed.sel = t.id;
      ed.link = {};
      if (ni.button == 0 && !ro) {
        ed.drag = t.id;
        ed.dragStart = t.pos;
      }
      if (ni.doubleClicked) ed.focusName = ed.sel;
    }
    if (ed.drag == t.id && ni.held && ni.dragging) {
      Vec2 np{snap(ed.dragStart.x + double(ni.dx) / ed.cam.z), snap(ed.dragStart.y + double(ni.dy) / ed.cam.z)};
      if (np != t.pos) a.act("Переместить технологию", [&](Tx& tx) { tx.tech(t.id).pos = np; }, {.coalesce = "tt-move:" + std::to_string(t.id), .coalesceSec = 3600});
      ui::setCursor(platform::Cursor::Grabbing);
    }
    if (ed.drag == t.id && !ni.held) {
      ed.drag = 0;
      a.store.endCoalesce();
    }
    if (ni.rightClicked) {
      ed.menuTech = t.id;
      ed.menuLink = {};
      openMenu = true;
    }
    if (ni.hovered && !ed.drag && !ed.conn) ui::setCursor(platform::Cursor::Hand);
    // «Галочка»
    if (ci.hovered && !ro) ui::setCursor(platform::Cursor::Hand);
    if (ci.clicked && !ro) {
      ed.sel = t.id;
      ed.link = {};
      bool on = !t.studied;
      a.act(on ? "Отметить изученной" : "Снять отметку изучения", [&](Tx& tx) { rules::setStudied(tx, t.id, on); });
    }
    // Связи от гнёзд
    if ((oi2.hovered || ii.hovered) && !ro) ui::setCursor(platform::Cursor::Crosshair);
    if (oi2.pressed && !ro) {
      ed.conn = t.id;
      ed.connOut = true;
    }
    if (ii.pressed && !ro) {
      ed.conn = t.id;
      ed.connOut = false;
    }
    if (ed.conn == t.id && ((ed.connOut && oi2.released) || (!ed.connOut && ii.released))) connReleased = true;
  }
  // Кнопку отпустили вне карточки (перетаскивание прервано) — закончить.
  if (ed.drag && !mouse.down[0]) {
    ed.drag = 0;
    a.store.endCoalesce();
  }
  // Цель связи под указателем.
  Id dropTarget = 0;
  bool dropBad = false;
  std::string dropWhy;
  if (ed.conn) {
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
      const TN& t = ts[*it];
      if (t.id == ed.conn) continue;
      if (mw.x >= t.pos.x - 10 && mw.x <= t.pos.x + kW + 10 && mw.y >= t.pos.y - 10 && mw.y <= t.pos.y + kH + 10) {
        dropTarget = t.id;
        break;
      }
    }
    if (dropTarget) {
      Id tech = ed.connOut ? dropTarget : ed.conn, pre = ed.connOut ? ed.conn : dropTarget;
      const TN* tt = find(ts, tech);
      if (tt && std::find(tt->prereqs.begin(), tt->prereqs.end(), pre) != tt->prereqs.end()) {
        dropBad = true;
        dropWhy = "Уже связаны";
      } else if (rules::wouldCycle(w, tech, pre)) {
        dropBad = true;
        dropWhy = "Связь создаст цикл";
      }
    }
    ui::setCursor(platform::Cursor::Crosshair);
    if (connReleased || !mouse.down[0]) {
      if (dropTarget) {
        Id tech = ed.connOut ? dropTarget : ed.conn, pre = ed.connOut ? ed.conn : dropTarget;
        const TN* tt = find(ts, tech);
        bool linked = tt && std::find(tt->prereqs.begin(), tt->prereqs.end(), pre) != tt->prereqs.end();
        if (!linked && a.act("Связать технологии", [&](Tx& tx) { rules::setPrereq(tx, tech, pre, true); })) {
          ed.link = {pre, tech};
          ed.sel = 0;
        }
      }
      ed.conn = 0;
    }
  }
  // Связь под указателем (только над фоном).
  LinkSel hoverLink;
  if (bg.hovered && !ed.conn && !ed.drag && !(ed.pan.on && ed.bgDragged)) {
    double best = 8 / ed.cam.z;
    for (const TN& t : ts)
      for (Id p : t.prereqs) {
        const TN* pt = find(ts, p);
        if (!pt) continue;
        double d = curveDistance(curve({pt->pos.x + kW, pt->pos.y + kH * 0.5}, {t.pos.x, t.pos.y + kH * 0.5}), mw);
        if (d < best) {
          best = d;
          hoverLink = {p, t.id};
        }
      }
    if (hoverLink) ui::setCursor(platform::Cursor::Hand);
  }
  // Фон: панорама, масштаб, щелчок, двойной щелчок, меню.
  bool overCanvas = bg.hovered || hoverNode || hoverCheck || hoverOut || hoverIn || ed.pan.on;
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
    if (Id nid = createAt(a, faction, mw)) {
      ed.sel = nid;
      ed.link = {};
      ed.focusName = ed.sel;
    }
  }
  if (bg.rightClicked) {
    ed.menuTech = 0;
    ed.menuLink = hoverLink;
    ed.menuAt = mw;
    openMenu = true;
  }
  if (openMenu) ui::openContextMenu("ctx");
  if (ui::beginMenu("ctx")) {
    if (const TN* t = find(ts, ed.menuTech)) {
      ui::menuHeader(t->name.empty() ? std::string("Без названия") : t->name);
      bool st = t->studied;
      Id tid = t->id;
      if (ui::menuItem("Изучена", {.icon = "check", .checked = st, .disabled = ro}))
        a.act(st ? "Снять отметку изучения" : "Отметить изученной", [&](Tx& tx) { rules::setStudied(tx, tid, !st); });
      if (t->research) {
        if (ui::menuItem("Остановить исследование", {.icon = "close", .disabled = ro})) a.act("Остановить исследование", [&](Tx& tx) { rules::stopResearch(tx, tid); });
      } else if (!t->studied) {
        if (ui::menuItem("Начать исследование", {.icon = "play", .disabled = ro || t->st != St::Available}))
          a.act("Начать исследование", [&](Tx& tx) { rules::startResearch(tx, tid); });
      }
      ui::menuSeparator();
      if (ui::menuItem("Удалить", {.icon = "trash", .shortcut = {Key::Delete, 0}, .danger = true, .disabled = ro})) askDelete(a, tid, t->name);
    } else if (ed.menuLink) {
      ui::menuHeader("Зависимость");
      if (ui::menuItem("Удалить связь", {.icon = "unlink", .shortcut = {Key::Delete, 0}, .danger = true, .disabled = ro})) removeLink(a, ed.menuLink);
    } else {
      if (ui::menuItem("Новая технология здесь", {.icon = "plus", .disabled = ro})) {
        if (Id nid = createAt(a, faction, ed.menuAt)) {
          ed.sel = nid;
          ed.focusName = ed.sel;
        }
      }
      if (ui::menuItem("Расставить автоматически", {.icon = "wand", .disabled = ro || ts.size() < 2}))
        if (a.act("Расставить дерево технологий", [&](Tx& tx) { rules::autoLayout(tx, faction); })) ed.refit = true;
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
        removeLink(a, ed.link);
        ed.link = {};
      } else if (const TN* t = find(ts, ed.sel)) {
        askDelete(a, t->id, t->name);
      }
    }
    // Стрелки — переход по дереву, если клавиатура не у виджета панели (числа, списки).
    if (const TN* cur = find(ts, ed.sel); cur && ui::keyboardFocus() == 0) {
      Id nx = 0;
      if (ui::shortcut({Key::Left, 0})) nx = neighbor(ts, *cur, -1, 0);
      else if (ui::shortcut({Key::Right, 0})) nx = neighbor(ts, *cur, 1, 0);
      else if (ui::shortcut({Key::Up, 0})) nx = neighbor(ts, *cur, 0, -1);
      else if (ui::shortcut({Key::Down, 0})) nx = neighbor(ts, *cur, 0, 1);
      if (nx) {
        ed.sel = nx;
        ed.revealSel = true;
      }
    }
  }
  if (ed.revealSel) {
    if (const TN* t = find(ts, ed.sel)) reveal(ed.cam, canvas, Box2{t->pos.x, t->pos.y, t->pos.x + kW, t->pos.y + kH}, true);
    ed.revealSel = false;
  }

  // ---- холст: сцена
  auto sc = std::make_shared<Scene>();
  sc->cam = ed.cam;
  sc->k = ink();
  sc->readOnly = ro;
  for (const TN& t : ts)
    for (Id p : t.prereqs) {
      const TN* pt = find(ts, p);
      if (!pt) continue;
      EdgeD e;
      e.k = curve({pt->pos.x + kW, pt->pos.y + kH * 0.5}, {t.pos.x, t.pos.y + kH * 0.5});
      LinkSel ls{p, t.id};
      e.sel = ed.link == ls;
      e.hot = hoverLink == ls;
      Color base = pt->studied ? (t.studied ? sc->k.success.alpha(0.75f) : t.st == St::Research ? sc->k.info.alpha(0.9f) : sc->k.accent.alpha(0.85f))
                               : sc->k.textMuted.alpha(0.55f);
      if (!t.match || !pt->match) base = base.alpha(0.3f);
      e.col = e.sel ? sc->k.accent : e.hot ? base.lighten(0.25f).alpha(1.f) : base;
      e.width = e.sel || e.hot ? 3.f : 2.f;
      sc->edges.push_back(e);
    }
  for (size_t oi : order) {
    const TN& t = ts[oi];
    Card cd;
    cd.t = t;
    cd.bonus = bonusText(w, t);
    if (t.st == St::Locked && !t.missing.empty()) cd.need = "Нужно: " + techNames(ts, t.missing);
    cd.sel = ed.sel == t.id;
    cd.hover = hoverNode == t.id || hoverCheck == t.id || hoverOut == t.id || hoverIn == t.id;
    cd.dim = !t.match;
    cd.drop = dropTarget == t.id;
    cd.dropBad = dropBad;
    cd.checkHot = hoverCheck == t.id;
    cd.outHot = hoverOut == t.id || (ed.conn == t.id && ed.connOut);
    cd.inHot = hoverIn == t.id || (ed.conn == t.id && !ed.connOut);
    cd.hasIn = !t.prereqs.empty();
    for (const TN& o : ts)
      if (std::find(o.prereqs.begin(), o.prereqs.end(), t.id) != o.prereqs.end()) cd.hasOut = true;
    sc->cards.push_back(std::move(cd));
  }
  if (ed.conn) {
    if (const TN* src = find(ts, ed.conn)) {
      sc->pending = true;
      Vec2 from = ed.connOut ? Vec2{src->pos.x + kW, src->pos.y + kH * 0.5} : Vec2{src->pos.x, src->pos.y + kH * 0.5};
      Vec2 to = mw;
      if (const TN* tg = find(ts, dropTarget); tg && !dropBad) to = ed.connOut ? Vec2{tg->pos.x, tg->pos.y + kH * 0.5} : Vec2{tg->pos.x + kW, tg->pos.y + kH * 0.5};
      sc->pend = ed.connOut ? curve(from, to) : curve(to, from);
      sc->pendCol = dropTarget ? (dropBad ? sc->k.danger : sc->k.success) : sc->k.accent;
    }
  }
  ui::custom(canvas, [sc](gfx::Canvas& c, RectF dev, float scale) { renderScene(*sc, c, dev, scale); });
  ui::draw::rectStroke(canvas, th.border, 10, 1);

  // ---- поверх холста
  ui::draw::pushClip(canvas);
  if (ts.empty()) {
    int act = canvasEmpty(canvas, "tech-tree", guild ? "У гильдии пока нет технологий." : "Дерево технологий пусто.", "Новая технология", "plus", ro,
                          "Скопировать дерево другой фракции", "copy", "tt.empty");
    if (act == 1) {
      if (Id nid = createAt(a, faction, {kW * 0.5, kH * 0.5})) {
        ed.sel = nid;
        ed.focusName = ed.sel;
        ed.refit = true;
      }
    } else if (act == 2) {
      ui::openPopup("copyfrom");   // якорь — кнопка пустого холста
    }
  }
  // Кнопка удаления у выбранной связи.
  if (ed.link && !ro) {
    const TN* pt = find(ts, ed.link.pre);
    const TN* tt = find(ts, ed.link.tech);
    if (pt && tt) {
      Vec2 mid = curveAt(curve({pt->pos.x + kW, pt->pos.y + kH * 0.5}, {tt->pos.x, tt->pos.y + kH * 0.5}), 0.5);
      gfx::Pt sp = toScreen(ed.cam, canvas, mid);
      if (canvas.inset(14).contains(sp.x, sp.y)) {
        RectF br{std::round(sp.x - 15), std::round(sp.y - 15), 30, 30};
        ui::draw::shadow(br, 15, 12, th.shadow, 3);
        ui::draw::rect(br, th.surface1, 15);
        ui::draw::rectStroke(br, th.danger.alpha(0.6f), 15, 1.2f);
        ui::at(br);
        if (ui::iconButton("unlink", "Удалить связь", {.shortcut = {Key::Delete, 0}, .tone = ui::Tone::Danger})) {
          removeLink(a, ed.link);
          ed.link = {};
        }
        a.markUi("tt.link.delete");
      }
    }
  }
  // Подпись к недопустимой связи у указателя.
  if (ed.conn && dropBad && !dropWhy.empty()) {
    float tw = ui::measure(dropWhy, ui::Font::Small) + 30;
    RectF r{mouse.x + 14, mouse.y + 14, tw, 24};
    ui::draw::rect(r, th.surface1, 12);
    ui::draw::rectStroke(r, th.danger.alpha(0.7f), 12, 1);
    ui::draw::icon("warning", RectF{r.x + 8, r.y + 5, 14, 14}, th.danger);
    ui::draw::text(dropWhy, RectF{r.x + 25, r.y, tw - 28, r.h}, ui::Font::Small, th.danger);
  }
  if (!ts.empty()) {
    zoomBar(a, ed.cam, canvas, bounds, "tt.zoom");
    std::vector<MiniItem> mi;
    for (const TN& t : ts) mi.push_back({Box2{t.pos.x, t.pos.y, t.pos.x + kW, t.pos.y + kH}, stColor(sc->k, t.st).alpha(t.match ? 0.85f : 0.3f), t.id == ed.sel});
    minimap(ed.cam, canvas, bounds, mi, "tt.mini");
  }
  ui::draw::popClip();
  // Меню «Скопировать дерево» (из панели инструментов или пустого холста).
  copyMenu(a, faction, !ts.empty());

  // ---- панель свойств
  {
    ui::draw::rect(side, th.surface2, th.radiusCard);
    ui::draw::rectStroke(side, th.border, th.radiusCard, 1);
    ui::Area ar(side.inset(14, 12), 0);
    ui::Scroll scroll("side");
    if (const TN* t = find(ts, ed.sel)) sideTech(a, ed, ts, *t);
    else if (ed.link) sideLink(a, ed, ts);
    else sideOverview(a, ed, faction, ts);
  }

  // ---- карточка сведений при наведении
  Id tipId = (hoverNode && !ed.drag && !ed.conn && !ui::anyModalOpen()) ? hoverNode : 0;
  if (hoverDelay(tipId ? hash64("tt") ^ tipId : 0) && tipId) {
    if (const TN* t = find(ts, tipId)) {
      Tip tip;
      tip.title = t->name.empty() ? "Без названия" : t->name;
      tip.subtitle = std::string(stName(t->st)) + " · " + nTurns(t->turns) + " изучения";
      tip.accent = stColor(sc->k, t->st);
      tip.icon = stIcon(t->st);
      if (!trim(t->desc).empty()) tip.lines.push_back({"", t->desc, th.textDim, true});
      for (Id mid : t->mods)
        if (const Modifier* m = w.modifier(mid))
          for (int fx = 0; fx < kFxCount; fx++)
            if (m->has(Fx(fx)) && m->fx[size_t(fx)] != 0)
              tip.lines.push_back({schema::effect(Fx(fx)).icon, w::effectText(Fx(fx), m->fx[size_t(fx)]),
                                   w::effectGood(Fx(fx), m->fx[size_t(fx)]) ? th.success : th.danger});
      if (t->research || (t->progress > 0 && !t->studied))
        tip.lines.push_back({"hourglass", "Пройдено " + std::to_string(t->progress) + " из " + std::to_string(t->turns) + ", осталось " + nTurns(turnsLeft(t->turns, t->progress)), th.info});
      if (!t->prereqs.empty()) tip.lines.push_back({"link", "Требует: " + techNames(ts, t->prereqs, 4), Color(0, 0, 0, 0), true});
      if (!t->missing.empty()) tip.lines.push_back({"lock", "Не изучены: " + techNames(ts, t->missing, 4), th.warning, true});
      std::vector<Id> deps;
      for (const TN& o : ts)
        if (std::find(o.prereqs.begin(), o.prereqs.end(), t->id) != o.prereqs.end()) deps.push_back(o.id);
      if (!deps.empty()) tip.lines.push_back({"arrow-right", "Открывает: " + techNames(ts, deps, 4), Color(0, 0, 0, 0), true});
      tipCard(tip, toScreen(ed.cam, canvas, t->pos.x, t->pos.y, kW, kH), canvas);
    }
  }
}

EditorReg regTechTree({"techtree", "Дерево технологий", drawTechTree, "tech-tree"});

// Команда палитры: дерево технологий выделенной фракции (или первого государства).
Id commandFaction(App& a) {
  const World& w = a.world();
  if (a.ui.sel.type == SelType::Faction && w.faction(a.ui.sel.id)) return a.ui.sel.id;
  if (a.ui.sel.type == SelType::Province)
    if (const Province* p = w.province(a.ui.sel.id); p && w.faction(p->owner)) return p->owner;
  Id first = 0;
  w.factions.each([&](const Faction& f) {
    if (!first && f.isState()) first = f.id;
  });
  return first;
}
CommandReg cmdTechTree({"trees.tech", "Дерево технологий", "tech-tree", nullptr, [](App& a) { openTechTree(a, commandFaction(a)); },
                        [](App& a) { return a.ui.screen == Screen::Editor && commandFaction(a) != 0; }, false, "Вид"});

}  // namespace

void openTechTree(App& a, Id faction, Id tech) {
  pending() = Pending{faction, tech};
  a.openEditor("techtree", faction);
}

}  // namespace rg::app
