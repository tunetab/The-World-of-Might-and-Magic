// Regnum — демонстрационный мир (вымышленный пример для тестов и снимков; не канон кампании). См. demo_world.h.
#include "map/demo_world.h"

#include <mutex>

#include "core/io.h"
#include "geo/ops.h"
#include "rules/rules.h"

namespace rg::map {

namespace {

using Poly = std::vector<Vec2>;

// ================================================================ геометрия: диаграмма Вороного с «природными» границами
Poly clipHalf(const Poly& p, Vec2 n, double c) {  // оставить n·x ≤ c
  Poly out;
  const size_t m = p.size();
  for (size_t i = 0; i < m; i++) {
    const Vec2 a = p[i], b = p[(i + 1) % m];
    const double da = n.dot(a) - c, db = n.dot(b) - c;
    if (da <= 0) out.push_back(a);
    if ((da < 0 && db > 0) || (da > 0 && db < 0)) out.push_back(a + (b - a) * (da / (da - db)));
  }
  return out;
}

Poly voronoiCell(const std::vector<Vec2>& seeds, size_t i, const Box2& box) {
  Poly p{{box.x0, box.y0}, {box.x1, box.y0}, {box.x1, box.y1}, {box.x0, box.y1}};
  for (size_t j = 0; j < seeds.size() && !p.empty(); j++) {
    if (j == i) continue;
    const Vec2 n = seeds[j] - seeds[i];
    p = clipHalf(p, n, n.dot((seeds[i] + seeds[j]) * 0.5));
  }
  // привязка вершин к сетке 1/1000: общие вершины соседних ячеек совпадают точно
  Poly out;
  for (Vec2 v : p) {
    const Vec2 q{std::round(v.x * 1000) / 1000, std::round(v.y * 1000) / 1000};
    if (out.empty() || dist2(out.back(), q) > 1e-6) out.push_back(q);
  }
  while (out.size() > 1 && dist2(out.front(), out.back()) <= 1e-6) out.pop_back();
  return out;
}

i64 qz(double v) { return i64(std::llround(v * 1000)); }

double noise(u64 seed) { return (Rng(seed).next() >> 11) * (2.0 / 9007199254740992.0) - 1.0; }

// Промежуточные точки извилистой границы a -> b (без концов): смещение середин, ключ — сами концы,
// поэтому соседние ячейки получают одну и ту же линию.
void wiggle(Vec2 a, Vec2 b, u64 seed, int depth, double amp, std::vector<Vec2>& out) {
  if (depth == 0) return;
  const Vec2 d = b - a;
  const Vec2 m = (a + b) * 0.5 + d.perp() * (amp * noise(seed));
  wiggle(a, m, hashMix(seed, 1), depth - 1, amp * 0.8, out);
  out.push_back(m);
  wiggle(m, b, hashMix(seed, 2), depth - 1, amp * 0.8, out);
}

Poly naturalize(const Poly& cell, const Box2& frame) {
  Poly out;
  const size_t n = cell.size();
  for (size_t i = 0; i < n; i++) {
    const Vec2 a = cell[i], b = cell[(i + 1) % n];
    out.push_back(a);
    const bool onFrame = (std::fabs(a.x - b.x) < 1e-9 && (std::fabs(a.x - frame.x0) < 1e-6 || std::fabs(a.x - frame.x1) < 1e-6)) ||
                         (std::fabs(a.y - b.y) < 1e-9 && (std::fabs(a.y - frame.y0) < 1e-6 || std::fabs(a.y - frame.y1) < 1e-6));
    const double len = dist(a, b);
    if (onFrame || len < 40) continue;
    const bool fwd = std::make_pair(qz(a.x), qz(a.y)) < std::make_pair(qz(b.x), qz(b.y));
    const Vec2 p = fwd ? a : b, q = fwd ? b : a;
    u64 seed = hashMix(hashMix(u64(qz(p.x)), u64(qz(p.y))), hashMix(u64(qz(q.x)), u64(qz(q.y))));
    std::vector<Vec2> mid;
    wiggle(p, q, seed, len > 260 ? 4 : 3, 0.16, mid);
    if (!fwd) std::reverse(mid.begin(), mid.end());
    for (Vec2 v : mid) out.push_back(v);
  }
  return out;
}

bool segCross(Vec2 a, Vec2 b, Vec2 c, Vec2 d) {
  auto orient = [](Vec2 p, Vec2 q, Vec2 r) { return (q - p).cross(r - p); };
  const double d1 = orient(c, d, a), d2 = orient(c, d, b), d3 = orient(a, b, c), d4 = orient(a, b, d);
  return ((d1 > 0) != (d2 > 0)) && ((d3 > 0) != (d4 > 0));
}

bool simple(const Poly& p) {
  const size_t n = p.size();
  for (size_t i = 0; i < n; i++)
    for (size_t j = i + 2; j < n; j++) {
      if (i == 0 && j == n - 1) continue;
      if (segCross(p[i], p[(i + 1) % n], p[j], p[(j + 1) % n])) return false;
    }
  return true;
}

// ================================================================ содержание
struct StateDef {
  const char* name;
  u32 color;
  FlagPattern pattern;
  u32 c0, c1, c2;
  const char* emblem;
  u32 emblemColor;
  int government;            // каталог форм правления (1..6)
  const char* ruler;
  const char* title;
  std::vector<Box2> land;    // области провинций
  int count;
  std::vector<const char*> provinces;  // первая — столичная
  const char* capitalCity;
};

const std::vector<StateDef>& states() {
  static const std::vector<StateDef> s = {
      {"Королевство Альмарин", 0xc23b2e, FlagPattern::Chief, 0xc23b2e, 0xf1e3bd, 0x1d2333, "lion", 0xf1d27a, 1, "Эдвин Альмарский", "Король",
       {Box2(2280, 940, 3330, 1900)}, 8,
       {"Альмарская марка", "Вереск", "Тальвин", "Красный Брод", "Ольховец", "Серая Гавань", "Дубрава", "Медвежий Лог"}, "Альмара"},
      {"Северный союз Хельдвиг", 0x2f6fd1, FlagPattern::Cross, 0x2f6fd1, 0xf4f1e8, 0xd9a441, "", 0, 5, "Ингвар Седобородый", "Верховный ярл",
       {Box2(2620, 120, 3330, 940), Box2(3330, 120, 4150, 1520)}, 9,
       {"Хельдгард", "Снежная марка", "Вальдра", "Ледяной кряж", "Фьорн", "Сосновец", "Белый порог", "Ярн", "Волчья падь"}, "Хельдгард"},
      {"Империя Валь-Кетра", 0x7b3fb5, FlagPattern::Saltire, 0x2a1d3d, 0x9b6ad6, 0xe8d9ff, "eagle", 0xf2e3b3, 2, "Аврелиана IV", "Императрица",
       {Box2(4150, 100, 5720, 1500)}, 9,
       {"Кетра", "Аметистовый дол", "Скайр", "Ниссен", "Мраморный мыс", "Вороний край", "Тарн-Кетра", "Лиран", "Пурпурный берег"}, "Кетра"},
      {"Республика Корвен", 0xe07b24, FlagPattern::V3, 0xe07b24, 0xf6efe0, 0x2b4a7a, "tower", 0x2b4a7a, 3, "Мариус Вейл", "Первый консул",
       {Box2(3240, 1880, 4240, 2620)}, 6, {"Корвен", "Медный тракт", "Ривель", "Устье Сарны", "Каменец", "Лисий холм"}, "Корвен"},
      {"Княжество Мирель", 0x1f9e89, FlagPattern::Bend, 0x1f9e89, 0xf2e8c9, 0x14574c, "tree", 0xf2e8c9, 1, "Лиана Мирельская", "Княгиня",
       {Box2(2880, 2620, 4480, 3160)}, 6, {"Мирель", "Зелёный плёс", "Тенистый лес", "Озёрный край", "Ясень", "Ивовый брод"}, "Мирель"},
      {"Вольные города Ольсты", 0xd8ab2a, FlagPattern::Quarters, 0xd8ab2a, 0x1f3550, 0x000000, "ship", 0xfff4d6, 5, "Гердт Ольстен", "Бургомистр",
       {Box2(4500, 1740, 5480, 3000)}, 6, {"Ольста", "Янтарный порт", "Солеварня", "Пристань Гирд", "Торжок", "Маячный мыс"}, "Ольста"},
      {"Орда Таргаш", 0x6e8c2e, FlagPattern::Border, 0x6e8c2e, 0x2b2418, 0x000000, "wolf", 0xe9dcb0, 6, "Бор-Хан Таргаш", "Великий хан",
       {Box2(3260, 3330, 5580, 4300)}, 8,
       {"Таргаш", "Пыльная степь", "Курган", "Рыжие холмы", "Ковыльное поле", "Сухой овраг", "Каменный идол", "Ветряной перевал"}, "Таргаш"},
      {"Теократия Солмар", 0xb3477d, FlagPattern::Pale, 0xf3e9dc, 0xb3477d, 0x000000, "sun", 0xf6d36b, 4, "Селестин Светлый", "Верховный иерарх",
       {Box2(5690, 1030, 7920, 2780)}, 9,
       {"Солмар", "Храм Зари", "Светлая долина", "Путь пилигримов", "Лазурь", "Белые ключи", "Келья", "Рассветный берег", "Звонница"}, "Солмар"},
  };
  return s;
}

struct Unowned { Box2 box; int count; };
const std::vector<Unowned>& unownedAreas() {
  static const std::vector<Unowned> u = {{Box2(5690, 2780, 7920, 4260), 4}, {Box2(1540, 1720, 2380, 3120), 2}};
  return u;
}
const char* const kUnownedNames[] = {"Дикие земли", "Туманный перешеек", "Пустошь Эрн", "Забытый край", "Глухомань", "Сумрачный лес"};

struct SeaDef { const char* name; Vec2 at; };
const std::vector<SeaDef>& seas() {
  static const std::vector<SeaDef> s = {{"Внутреннее море", {4300, 1660}}, {"Янтарное море", {4700, 3150}},  {"Штормовой пролив", {2650, 2250}},
                                        {"Южный залив", {2950, 3800}},     {"Лазурная бухта", {5560, 1700}}, {"Восточное взморье", {6250, 4380}}};
  return s;
}

// Точка на суше (с запасом от берега) или в открытом море.
bool landAt(const Basemap& bm, Vec2 p, double margin) {
  for (Vec2 d : {Vec2(0, 0), Vec2(margin, 0), Vec2(-margin, 0), Vec2(0, margin), Vec2(0, -margin)})
    if (bm.isOcean(p + d) || p.x + d.x < 0 || p.y + d.y < 0 || p.x + d.x > bm.width() || p.y + d.y > bm.height()) return false;
  return true;
}
bool seaAt(const Basemap& bm, Vec2 p, double margin) {
  for (Vec2 d : {Vec2(0, 0), Vec2(margin, 0), Vec2(-margin, 0), Vec2(0, margin), Vec2(0, -margin)})
    if (!bm.isOcean(p + d)) return false;
  return true;
}

struct Seed {
  Vec2 p;
  int group = -1;    // государство; −2 — без владельца; −3 — море; −1 — заполнитель
  int index = 0;     // номер в группе
};

std::vector<Seed> makeSeeds(const Basemap& bm) {
  std::vector<Seed> seeds;
  Rng rng(0x5eed2026);
  auto farFrom = [&](Vec2 p, double d) {
    for (const Seed& s : seeds)
      if (dist2(s.p, p) < d * d) return false;
    return true;
  };
  auto sample = [&](const std::vector<Box2>& boxes, int count, int group) {
    double area = 0;
    for (const Box2& b : boxes) area += b.w() * b.h();
    double minD = std::sqrt(area / count) * 0.62;
    int got = 0;
    for (int attempt = 0; got < count && attempt < 60000; attempt++) {
      if (attempt % 6000 == 5999) minD *= 0.85;
      double pick = rng.uniform() * area;
      const Box2* b = &boxes[0];
      for (const Box2& x : boxes) {
        if (pick <= x.w() * x.h()) { b = &x; break; }
        pick -= x.w() * x.h();
      }
      const Vec2 p{b->x0 + rng.uniform() * b->w(), b->y0 + rng.uniform() * b->h()};
      if (!landAt(bm, p, 28) || !farFrom(p, minD)) continue;
      seeds.push_back({p, group, got++});
    }
  };
  for (size_t i = 0; i < states().size(); i++) sample(states()[i].land, states()[i].count, int(i));
  int un = 0;
  for (const Unowned& u : unownedAreas()) {
    const size_t before = seeds.size();
    sample({u.box}, u.count, -2);
    for (size_t k = before; k < seeds.size(); k++) seeds[k].index = un++;
  }
  int si = 0;
  for (const SeaDef& s : seas()) {
    Vec2 p = s.at;
    for (int r = 0; r < 60 && !seaAt(bm, p, 40); r++) {
      const double a = r * 2.399963, rad = 15.0 * r;
      p = s.at + Vec2(std::cos(a), std::sin(a)) * rad;
    }
    seeds.push_back({p, -3, si++});
  }
  // Заполнители ограничивают ячейки: свободная суша и открытое море остаются не назначенными.
  const double S = 620;
  for (double y = S * 0.5; y < bm.height(); y += S)
    for (double x = S * 0.5; x < bm.width(); x += S) {
      const Vec2 p{x + (rng.uniform() - 0.5) * S * 0.4, y + (rng.uniform() - 0.5) * S * 0.4};
      if (farFrom(p, S * 0.78)) seeds.push_back({p, -1, 0});
    }
  return seeds;
}

Id catalogAdd(Tx& tx, rules::CatalogList list, const char* name, u32 color, const char* icon) {
  const Id id = rules::addCatalogItem(tx, list, name);
  for (CatalogItem& c : rules::catalogList(tx.catalogs(), list))
    if (c.id == id) {
      c.color = Color::hex(color);
      c.icon = icon;
    }
  return id;
}

double pick(Rng& r, double lo, double hi) { return lo + (hi - lo) * r.uniform(); }

World build(const Basemap& bm) {
  if (!bm.loaded()) fail("Базовая карта не загружена — демонстрационный мир не построить");
  World w = newWorld("Демонстрационный мир");
  {
    Tx tx(w);
    Meta& m = tx.meta();
    m.createdAt = m.updatedAt = "2026-10-01T00:00:00Z";
    m.basemap = bm.id();
    m.notes = "Вымышленный пример для тестов и снимков экрана редактора Regnum. Не канон кампании.";
    geo::initFromCoast(tx, bm.coast());
    w = std::move(tx).finish();
  }

  // ---------------------------------------------------------------- провинции
  const std::vector<Seed> seeds = makeSeeds(bm);
  std::vector<Vec2> pts;
  for (const Seed& s : seeds) pts.push_back(s.p);
  const Box2 frame(0, 0, bm.width(), bm.height());
  std::vector<std::vector<Id>> stateProv(states().size());
  std::vector<Id> unowned, seaProv;
  for (size_t i = 0; i < seeds.size(); i++) {
    const Seed& s = seeds[i];
    if (s.group == -1) continue;
    Poly cell = voronoiCell(pts, i, frame);
    if (cell.size() < 3) continue;
    Poly poly = naturalize(cell, frame);
    if (!simple(poly)) poly = cell;
    try {
      Tx tx(w);
      const Id pid = geo::createProvince(tx, poly, s.group == -3 ? Terrain::Sea : Terrain::Land, geo::EditOptions{1.0});
      w = std::move(tx).finish();
      if (s.group >= 0) stateProv[size_t(s.group)].push_back(pid);
      else if (s.group == -2) unowned.push_back(pid);
      else seaProv.push_back(pid);
    } catch (const UserError& e) {
      logWarn("Демонстрационный мир: ячейка %zu пропущена: %s", i, e.what());
    }
  }

  Tx tx(w);
  Rng rng(0xd3e0);
  using rules::CatalogList;

  // ---------------------------------------------------------------- справочники
  const Id rHuman = catalogAdd(tx, CatalogList::Races, "Люди", 0xd8b48c, "race");
  const Id rElf = catalogAdd(tx, CatalogList::Races, "Эльфы", 0x79b86a, "race");
  const Id rDwarf = catalogAdd(tx, CatalogList::Races, "Гномы", 0xb0855a, "race");
  const Id rOrc = catalogAdd(tx, CatalogList::Races, "Орки", 0x6f9443, "race");
  const Id rHalf = catalogAdd(tx, CatalogList::Races, "Полурослики", 0xe0b070, "race");
  const Id rTriton = catalogAdd(tx, CatalogList::Races, "Тритоны", 0x4fa3c7, "race");
  const u32 cultureColors[8] = {0xb8574a, 0x4f7fc0, 0x8a5cb8, 0xd08a3a, 0x3aa08f, 0xc9a640, 0x7a8f3a, 0xb85c8a};
  const char* cultureNames[8] = {"Альмарская", "Северная", "Кетрийская", "Корвенская", "Мирельская", "Портовая", "Степная", "Солмарская"};
  std::vector<Id> cultures;
  for (int i = 0; i < 8; i++) cultures.push_back(catalogAdd(tx, CatalogList::Cultures, cultureNames[i], cultureColors[i], "culture"));
  const Id relDawn = catalogAdd(tx, CatalogList::Religions, "Культ Зари", 0xe8b84a, "religion");
  const Id relOld = catalogAdd(tx, CatalogList::Religions, "Старые боги", 0x6f8f5a, "religion");
  const Id relMoon = catalogAdd(tx, CatalogList::Religions, "Лунный круг", 0x7f86c9, "religion");
  const Id relForge = catalogAdd(tx, CatalogList::Religions, "Пламя Горна", 0xc9643a, "religion");
  const Id resGem = catalogAdd(tx, CatalogList::Resources, "Самоцветы", 0xa35fc9, "gem");
  const Id resSalt = catalogAdd(tx, CatalogList::Resources, "Соль", 0xcfc9bb, "resource");
  const Id resSpice = catalogAdd(tx, CatalogList::Resources, "Пряности", 0xc96a3a, "resource");
  const Id resMithril = catalogAdd(tx, CatalogList::Resources, "Мифрил", 0x7fc4dd, "resource");
  const Id stateReligion[8] = {relDawn, relOld, relMoon, relDawn, relOld, relDawn, relForge, relDawn};
  const Id stateRace[8] = {rHuman, rHuman, rHuman, rHuman, rElf, rHalf, rOrc, rHuman};
  const Id minorRace[8] = {rHalf, rDwarf, rElf, rDwarf, rHuman, rTriton, rHuman, rElf};

  // ---------------------------------------------------------------- государства и правители
  std::vector<Id> st;
  for (size_t i = 0; i < states().size(); i++) {
    const StateDef& d = states()[i];
    const Id id = rules::createFaction(tx, FactionKind::State, d.name);
    st.push_back(id);
    Faction& f = tx.faction(id);
    f.color = Color::hex(d.color);
    f.flag.pattern = d.pattern;
    f.flag.colors = {Color::hex(d.c0), Color::hex(d.c1), Color::hex(d.c2)};
    f.flag.emblem = d.emblem;
    f.flag.emblemColor = Color::hex(d.emblemColor);
    f.culture = cultures[i];
    f.government = Id(d.government);
    f.religion = stateReligion[i];
    f.tax = 8 + double(i % 4) * 3;
    f.res[kGold] = 1500 + 450 * double(i);
    f.res[2] = 300 + 40 * double(i);
    f.res[3] = 200 + 25 * double(i);
    f.res[5] = 80 + 15 * double(i);
    f.notes = "Демонстрационное государство (не канон).";
    const Id ruler = rules::createCharacter(tx, id, d.ruler);
    tx.character(ruler).title = d.title;
    tx.character(ruler).hero = true;
    tx.character(ruler).upkeep = 25;
    tx.faction(id).ruler = ruler;
    tx.faction(id).rulerTitle = d.title;
  }
  // Советники и герои.
  const char* counselors[][3] = {
      {"Леди Ровена Кейн", "Сэр Освальд Брен", "Магистр Тиберий"}, {"Хальвар Мудрый", "Сигрун Остролистая", "Торстейн"},
      {"Кассиан Дор", "Северина Ларк", "Архимаг Илларион"},    {"Октавия Сол", "Децим Ферра", "Брут Каллен"},
      {"Эйлин Тенистая", "Лорин Ясный", "Мэйв"},             {"Ганс Ольстен", "Фрида Янтарь", "Ульф Солевар"},
      {"Шаман Ург", "Кара-Тай", "Гхаш Железный"},            {"Сестра Альба", "Брат Лукиан", "Мать Евлалия"}};
  const char* heroes[][2] = {{"Сэр Гарет Красный", "Лучница Нэйра"}, {"Бьорн Медведь", "Ульрика Ледяная"},     {"Легат Максен", "Чародейка Вира"},
                             {"Капитан Ривз", "Следопыт Лис"},      {"Ариэль Звёздная", "Кедр Молчаливый"},   {"Адмирал Корт", "Шкипер Ханна"},
                             {"Тарг Кровавый", "Вождь Мор"},        {"Паладин Ирис", "Инквизитор Ворн"}};
  std::vector<std::vector<Id>> heroIds(st.size());
  for (size_t i = 0; i < st.size(); i++) {
    const auto& cat = tx.w().catalogs->positions;
    for (int k = 0; k < 3; k++) {
      const Id ch = rules::createCharacter(tx, st[i], counselors[i][k]);
      tx.character(ch).upkeep = 8 + 2 * k;
      CouncilSeat seat;
      seat.id = tx.nextId(Seq::Council);
      seat.position = cat[size_t((i + size_t(k)) % cat.size())].name;
      seat.character = ch;
      tx.faction(st[i]).council.push_back(seat);
    }
    for (int k = 0; k < 2; k++) {
      const Id h = rules::createCharacter(tx, st[i], heroes[i][k]);
      tx.character(h).hero = true;
      tx.character(h).title = k == 0 ? "Полководец" : "Герой";
      tx.character(h).upkeep = 15;
      heroIds[i].push_back(h);
    }
  }

  // ---------------------------------------------------------------- владения и сведения провинций
  const Id resources[] = {2, 3, 4, 5, 6, resGem, resSalt, resSpice, resMithril};
  auto fs = geo::faces(w);
  auto areaOf = [&](Id p) { const geo::ProvinceShape* s = fs->shape(p); return s ? s->area : 0.0; };
  auto fillProvince = [&](Id pid, const std::string& name, Id owner, size_t si, bool capital) {
    Province& p = tx.province(pid);
    p.name = name;
    const double a = areaOf(pid);
    p.size = a > 260000 ? ProvSize::Large : a > 110000 ? ProvSize::Medium : ProvSize::Small;
    p.city = capital ? CityType::City : CityType(rng.range(0, 2));
    p.resource = resources[rng.range(0, 8)];
    p.resourceAmount = std::round(pick(rng, 8, 60));
    p.contentment = std::round(pick(rng, -55, 85));
    p.baseTrade = std::round(pick(rng, 6, capital ? 60 : 38)) * 20;  // сотни: налог ~10 % даёт десятки золота с провинции
    p.localTax = std::round(pick(rng, 0, 4));
    if (owner) {
      p.culture = rng.uniform() < 0.82 ? cultures[si] : cultures[size_t(rng.range(0, 7))];
      p.religion = rng.uniform() < 0.8 ? stateReligion[si] : (rng.uniform() < 0.5 ? relOld : relMoon);
      p.races = {RacePop{stateRace[si], i64(pick(rng, 18, 140)) * 1000}};
      if (rng.uniform() < 0.7) p.races.push_back(RacePop{minorRace[si], i64(pick(rng, 2, 40)) * 1000});
    } else {
      p.culture = rng.uniform() < 0.5 ? 0 : cultures[size_t(rng.range(0, 7))];
      p.religion = rng.uniform() < 0.5 ? 0 : relOld;
      p.races = {RacePop{rng.uniform() < 0.5 ? rOrc : rElf, i64(pick(rng, 2, 20)) * 1000}};
    }
    if (capital) p.capital = states()[si].capitalCity;
  };
  for (size_t i = 0; i < st.size(); i++) {
    auto& list = stateProv[i];
    std::stable_sort(list.begin(), list.end(), [&](Id a, Id b) { return areaOf(a) > areaOf(b); });
    for (size_t k = 0; k < list.size(); k++) {
      rules::setProvinceOwner(tx, list[k], st[i]);
      const auto& names = states()[i].provinces;
      fillProvince(list[k], k < names.size() ? names[k] : std::string(names[0]) + " " + std::to_string(k), st[i], i, k == 0);
    }
    if (!list.empty()) rules::setCapital(tx, st[i], list[0]);
  }
  for (size_t k = 0; k < unowned.size(); k++) fillProvince(unowned[k], kUnownedNames[k % 6], 0, 0, false);
  for (size_t k = 0; k < seaProv.size(); k++) tx.province(seaProv[k]).name = seas()[k % seas().size()].name;

  // Лорды провинций — советники и герои своих государств.
  for (size_t i = 0; i < st.size(); i++) {
    const auto& list = stateProv[i];
    for (size_t k = 1; k < list.size() && k <= heroIds[i].size(); k++) tx.province(list[k]).lord = heroIds[i][k - 1];
    if (!list.empty()) tx.province(list[0]).lord = tx.w().faction(st[i])->ruler;
  }

  // ---------------------------------------------------------------- модификаторы
  auto modifier = [&](const char* name, const char* icon, u32 color, std::initializer_list<std::pair<Fx, double>> fx) {
    const Id id = rules::createModifier(tx, name);
    Modifier& m = tx.modifier(id);
    m.icon = icon;
    m.color = Color::hex(color);
    for (auto [f, v] : fx) {
      m.fx[size_t(int(f))] = v;
      m.fxMask |= 1u << int(f);
    }
    return id;
  };
  const Id mFertile = modifier("Плодородные земли", "grain", 0x7fb069, {{Fx::PopGrowthPct, 2}, {Fx::TradePct, 10}});
  const Id mRoad = modifier("Торговый тракт", "route", 0xd9a441, {{Fx::TradeFlat, 40}});
  const Id mUnrest = modifier("Мятежные настроения", "rebellion", 0xc2412f, {{Fx::ContentmentPerTurn, -3}, {Fx::RebellionPct, 12}});
  const Id mDecree = modifier("Королевский указ", "scroll", 0x8c7ae6, {{Fx::IncomePct, 6}});
  const Id mDrill = modifier("Строевая муштра", "army", 0x6d7c8f, {{Fx::ArmyUpkeepPct, -10}});
  const Id mMines = modifier("Глубокие шахты", "pickaxe", 0x9aa0a8, {{Fx::ResourcePct, 25}, {Fx::Slots, 1}});
  tx.faction(st[0]).modifiers.push_back(mDecree);
  tx.faction(st[6]).modifiers.push_back(mDrill);
  for (size_t i = 0; i < st.size(); i++) {
    const auto& list = stateProv[i];
    if (list.size() > 2) tx.province(list[1]).modifiers.push_back(i % 2 ? mFertile : mRoad);
    if (list.size() > 4) tx.province(list[3]).modifiers.push_back(mMines);
  }

  // ---------------------------------------------------------------- постройки
  auto building = [&](Id owner, const char* name, const char* icon, BuildingCat cat, int levels, Id mod) {
    const Id id = rules::createBuilding(tx, owner, name);
    Building& b = tx.building(id);
    b.icon = icon;
    b.cat = cat;
    b.levels.clear();
    for (int l = 0; l < levels; l++) {
      BuildingLevel lv;
      lv.turns = 2 + l;
      lv.cost[kGold] = 150.0 * (l + 1);
      lv.cost[4] = 40.0 * (l + 1);
      if (mod) lv.modifiers.push_back(mod);
      b.levels.push_back(lv);
    }
    return id;
  };
  const Id bMarket = building(0, "Рынок", "coins", BuildingCat::Economic, 3, mRoad);
  const Id bBarracks = building(0, "Казармы", "army", BuildingCat::Military, 2, 0);
  const Id bForge = building(0, "Кузница", "hammer", BuildingCat::Industrial, 2, mMines);
  const Id bQuarter = building(0, "Жилой квартал", "house", BuildingCat::Residential, 2, mFertile);
  const Id bExchange = building(st[5], "Янтарная биржа", "trade", BuildingCat::Economic, 1, mRoad);
  for (size_t i = 0; i < st.size(); i++) {
    const auto& list = stateProv[i];
    for (size_t k = 0; k < list.size(); k++) {
      auto& b = tx.province(list[k]).buildings;
      if (k == 0) {
        b.push_back(ProvBuilding{bMarket, 2, false, 0});
        b.push_back(ProvBuilding{bBarracks, 1, false, 0});
        if (i == 5) b.push_back(ProvBuilding{bExchange, 1, false, 0});
      } else if (k % 3 == 1) {
        b.push_back(ProvBuilding{bForge, 1, false, 0});
      } else if (k % 3 == 2) {
        // Улучшение строится: уплачена цена уровня II (множитель стоимости демо-провинций — 1).
        b.push_back(ProvBuilding{bQuarter, 2, true, 1, tx.w().building(bQuarter)->levels[1].cost, st[i]});
      }
    }
  }

  // ---------------------------------------------------------------- технологии
  for (size_t i : {size_t(0), size_t(1), size_t(2)}) {
    const char* names[] = {"Обработка железа", "Арбалеты", "Тяжёлая конница", "Торговые гильдии", "Мореходство", "Осадные машины"};
    std::vector<Id> t;
    for (int k = 0; k < 6; k++) {
      const Id id = rules::createTech(tx, st[i], names[k]);
      tx.tech(id).turns = 2 + k;
      t.push_back(id);
    }
    tx.tech(t[3]).modifiers.push_back(mRoad);
    rules::setPrereq(tx, t[1], t[0], true);
    rules::setPrereq(tx, t[2], t[0], true);
    rules::setPrereq(tx, t[5], t[1], true);
    rules::setPrereq(tx, t[5], t[2], true);
    rules::setPrereq(tx, t[4], t[3], true);
    rules::setStudied(tx, t[0], true);
    rules::setStudied(tx, t[3], true);
    rules::startResearch(tx, t[1]);
    rules::autoLayout(tx, st[i]);
  }

  // ---------------------------------------------------------------- отношения
  rules::setRelation(tx, st[0], st[1], 72, RelStatus::Alliance);
  rules::setRelation(tx, st[1], st[2], -64, RelStatus::War);
  rules::setRelation(tx, st[0], st[2], -18, RelStatus::Neutral);
  rules::setRelation(tx, st[3], st[4], 58, RelStatus::Alliance);
  rules::setRelation(tx, st[6], st[3], -47, RelStatus::War);
  rules::setRelation(tx, st[6], st[4], -30, RelStatus::Neutral);
  rules::setRelation(tx, st[5], st[2], 22, RelStatus::Neutral);
  rules::setRelation(tx, st[5], st[3], 35, RelStatus::Neutral);
  rules::setRelation(tx, st[7], st[6], -12, RelStatus::Neutral);
  rules::setRelation(tx, st[7], st[5], 41, RelStatus::Neutral);

  // Оккупации на фронтах войн.
  auto nearest = [&](const std::vector<Id>& from, const std::vector<Id>& to) {
    Id best = 0;
    double bd = kInf;
    for (Id a : from) {
      const geo::ProvinceShape* sa = fs->shape(a);
      if (!sa) continue;
      for (Id b : to) {
        const geo::ProvinceShape* sb = fs->shape(b);
        if (!sb) continue;
        const double d = dist(sa->label, sb->label);
        if (d < bd) { bd = d; best = a; }
      }
    }
    return best;
  };
  std::vector<Id> occupied;
  auto occupy = [&](size_t victim, size_t occupier) {
    const Id p = nearest(stateProv[victim], stateProv[occupier]);
    if (!p || (!stateProv[victim].empty() && p == stateProv[victim][0])) return;
    rules::setOccupied(tx, p, st[occupier]);
    tx.province(p).contentment = -45;
    tx.province(p).modifiers.push_back(mUnrest);
    occupied.push_back(p);
  };
  occupy(1, 2);
  occupy(2, 1);
  occupy(3, 6);

  // ---------------------------------------------------------------- гильдии
  struct GuildDef { const char* name; u32 color; FlagPattern pat; u32 c0, c1; const char* emblem; size_t home; std::vector<size_t> zone; };
  const std::vector<GuildDef> guildDefs = {
      {"Янтарная лига", 0xe39b2d, FlagPattern::H2, 0xe39b2d, 0x3b2a12, "gem", 5, {5, 3, 2, 7}},
      {"Гильдия магов Аркана", 0x5a63d6, FlagPattern::Solid, 0x2c2f6e, 0x5a63d6, "eye", 2, {2, 1, 7}},
      {"Братство кузнецов", 0x9a4b2a, FlagPattern::Chevron, 0x9a4b2a, 0xd8cfc0, "hammer", 1, {1, 0, 6}},
  };
  std::vector<Id> guilds;
  for (const GuildDef& g : guildDefs) {
    const Id id = rules::createFaction(tx, FactionKind::Guild, g.name);
    Faction& f = tx.faction(id);
    f.color = Color::hex(g.color);
    f.flag.pattern = g.pat;
    f.flag.colors = {Color::hex(g.c0), Color::hex(g.c1), Color::hex(0xf2e3b3)};
    f.flag.emblem = g.emblem;
    f.flag.emblemColor = g.pat == FlagPattern::Chevron ? Color::hex(0x5a2a16) : Color::hex(0xf6ecd2);
    f.res[kGold] = 900;
    f.notes = "Демонстрационная гильдия (не канон).";
    rules::setHomeState(tx, id, st[g.home]);
    guilds.push_back(id);
  }
  const Id stateGuild = rules::createStateGuild(tx, st[0], "Альмарская торговая компания");
  {
    Faction& f = tx.faction(stateGuild);
    f.color = Color::hex(0x2a9fc4);
    f.flag.pattern = FlagPattern::Canton;
    f.flag.colors = {Color::hex(0x2a9fc4), Color::hex(0xc23b2e), Color::hex(0xf1e3bd)};
    f.flag.emblem = "key";
    f.res[kGold] = 1200;
  }
  guilds.push_back(stateGuild);
  const std::vector<std::vector<size_t>> zones = {guildDefs[0].zone, guildDefs[1].zone, guildDefs[2].zone, {0}};
  for (size_t gi = 0; gi < guilds.size(); gi++) {
    int hq = 0;
    for (size_t zi = 0; zi < zones[gi].size(); zi++) {
      const auto& list = stateProv[zones[gi][zi]];
      for (size_t k = 0; k < list.size(); k++) {
        if ((k + gi) % 2 == 1 && zi > 0) continue;
        const Province* p = tx.w().province(list[k]);
        double free = 100;
        for (const Influence& inf : p->influence) free -= inf.pct;
        const double pct = std::min(free, std::round(pick(rng, zi == 0 ? 22 : 8, zi == 0 ? 48 : 26)));
        if (pct >= 3) rules::setInfluence(tx, list[k], guilds[gi], pct);
        if (hq < 3 && (k == 0 || (zi == 0 && k == 2))) {
          rules::buildHq(tx, guilds[gi], list[k]);
          hq++;
        }
      }
    }
  }

  // ---------------------------------------------------------------- войска, флот, гарнизоны
  struct RowSet { Id inf, cav, ranged, casters, fleetLine, fleetFrig; };
  std::vector<RowSet> rows(st.size());
  // Численности строк подобраны под доходы государств, содержание — золото за одного воина (корабль) в ход:
  // большинство государств слегка в плюсе, Валь-Кетра (война, репарации) и Мирель (дань) — в минусе.
  struct ArmyDef { i64 inf, cav, ranged, casters, line, frig; };
  const ArmyDef armyDefs[8] = {
      {2600, 550, 650, 30, 2, 4},  {4000, 700, 600, 40, 5, 10}, {5200, 1100, 800, 100, 7, 14}, {3400, 550, 900, 60, 4, 10},
      {2000, 200, 1100, 70, 2, 6}, {2400, 300, 700, 30, 7, 22}, {6000, 2400, 700, 20, 1, 4},   {5500, 1150, 900, 160, 6, 12}};
  for (size_t i = 0; i < st.size(); i++) {
    const ArmyDef& d = armyDefs[i % 8];
    const bool horde = i == 6;  // орда: лёгкие пехота и конница дешевле
    rows[i].inf = rules::addArmyRow(tx, st[i], horde ? UnitType::LightInf : UnitType::MediumInf, {}, d.inf, horde ? 0.04 : 0.05);
    rows[i].cav = rules::addArmyRow(tx, st[i], horde ? UnitType::LightCav : UnitType::HeavyCav, {}, d.cav, horde ? 0.12 : 0.2);
    rows[i].ranged = rules::addArmyRow(tx, st[i], UnitType::Ranged, {}, d.ranged, 0.08);
    rows[i].casters = rules::addArmyRow(tx, st[i], UnitType::Casters, {}, d.casters, 0.5);
    rows[i].fleetLine = rules::addFleetRow(tx, st[i], ShipType::ShipOfLine, {}, d.line, 6);
    rows[i].fleetFrig = rules::addFleetRow(tx, st[i], ShipType::Frigate, {}, d.frig, 2.5);
  }
  // Гарнизоны в столицах.
  for (size_t i = 0; i < st.size(); i++)
    if (!stateProv[i].empty()) rules::setGarrison(tx, stateProv[i][0], rows[i].inf, 600);

  auto place = [&](ArmyKind kind, Vec2 near) -> Vec2 {
    auto p = rules::findFreeSpot(tx.w(), kind, near);
    if (!p) fail("Демонстрационный мир: нет места для войска");
    return *p;
  };
  auto labelOf = [&](Id pid) { const geo::ProvinceShape* s = fs->shape(pid); return s ? s->label : Vec2(4000, 2250); };
  auto between = [&](Id a, Id b, double t) { return labelOf(a) + (labelOf(b) - labelOf(a)) * t; };
  auto army = [&](size_t si, Vec2 near, i64 inf, i64 cav, i64 rng2, i64 cast) {
    const Id id = rules::createArmy(tx, ArmyKind::Army, st[si], place(ArmyKind::Army, near));
    if (inf) rules::setUnits(tx, id, st[si], rows[si].inf, inf);
    if (cav) rules::setUnits(tx, id, st[si], rows[si].cav, cav);
    if (rng2) rules::setUnits(tx, id, st[si], rows[si].ranged, rng2);
    if (cast) rules::setUnits(tx, id, st[si], rows[si].casters, cast);
    return id;
  };
  auto fleet = [&](size_t si, Vec2 near, i64 line, i64 frig) {
    const Id id = rules::createArmy(tx, ArmyKind::Fleet, st[si], place(ArmyKind::Fleet, near));
    if (line) rules::setUnits(tx, id, st[si], rows[si].fleetLine, line);
    if (frig) rules::setUnits(tx, id, st[si], rows[si].fleetFrig, frig);
    return id;
  };
  // Фронт Хельдвиг — Валь-Кетра.
  const Id hFront = nearest(stateProv[1], stateProv[2]), vFront = nearest(stateProv[2], stateProv[1]);
  const Id a1 = army(1, between(hFront, vFront, 0.25), 1800, 400, 500, 30);
  rules::setHero(tx, a1, heroIds[1][0], true);
  rules::setCommander(tx, a1, heroIds[1][0]);
  const Id a2 = army(2, between(vFront, hFront, 0.3), 2400, 600, 400, 60);
  rules::setHero(tx, a2, heroIds[2][0], true);
  rules::setCommander(tx, a2, heroIds[2][0]);
  // Союзное войско Альмарина и Хельдвига.
  const Id a3 = army(0, labelOf(stateProv[0][0]) + Vec2(90, 60), 1500, 500, 600, 20);
  rules::setHero(tx, a3, heroIds[0][0], true);
  rules::setCommander(tx, a3, heroIds[0][0]);
  const Id a4 = army(1, labelOf(stateProv[0][0]) + Vec2(-260, 180), 900, 200, 0, 0);
  rules::formAllied(tx, a3, a4);
  // Фронт Таргаш — Корвен.
  const Id tFront = nearest(stateProv[6], stateProv[3]), kFront = nearest(stateProv[3], stateProv[6]);
  army(6, between(tFront, kFront, 0.2), 3200, 1500, 600, 0);
  army(6, labelOf(stateProv[6][0]) + Vec2(-60, 40), 1100, 600, 0, 0);
  army(3, between(kFront, tFront, 0.2), 2000, 300, 700, 40);
  army(7, labelOf(stateProv[7][0]) + Vec2(40, 70), 1400, 350, 300, 80);
  army(4, labelOf(stateProv[4][0]) + Vec2(30, 50), 800, 0, 900, 50);
  army(5, labelOf(stateProv[5][0]) + Vec2(-40, 60), 700, 100, 300, 0);
  // Флоты.
  const auto seaAt2 = [&](size_t k) { return k < seaProv.size() ? labelOf(seaProv[k]) : Vec2(4300, 1660); };
  fleet(1, seaAt2(0) + Vec2(-120, -40), 4, 8);
  fleet(2, seaAt2(0) + Vec2(160, 30), 5, 10);
  fleet(5, seaAt2(1), 3, 12);
  const Id f1 = fleet(3, seaAt2(2) + Vec2(40, 0), 2, 6);
  const Id f2 = fleet(4, seaAt2(2) + Vec2(-200, 120), 1, 5);
  rules::formAllied(tx, f1, f2);
  fleet(7, seaAt2(4), 3, 6);

  // ---------------------------------------------------------------- торговые маршруты
  auto route = [&](std::vector<Vec2> ptsR, Id guild, const char* name) {
    const Id id = rules::createRoute(tx, ptsR, guild);
    tx.route(id).name = name;
    return id;
  };
  route({labelOf(stateProv[5][0]), seaAt2(1), labelOf(stateProv[6][0])}, guilds[0], "Янтарный путь");
  route({labelOf(stateProv[5][0]), seaAt2(4), labelOf(stateProv[7][0])}, guilds[0], "Восточный тракт");
  route({labelOf(stateProv[2][0]), seaAt2(0), labelOf(stateProv[3][0]), labelOf(stateProv[4][0])}, guilds[1], "Путь чародеев");
  route({labelOf(stateProv[0][0]), labelOf(stateProv[1][0])}, guilds[2], "Северная дорога");
  route({labelOf(stateProv[0][0]), seaAt2(2), labelOf(stateProv[4][0])}, 0, "Морской путь Альмарина");

  // ---------------------------------------------------------------- сделки, дань, репарации
  {
    Deal d;
    d.kind = DealKind::Trade;
    d.a = st[5];
    d.b = st[3];
    d.items.push_back(DealItem{DealSide::A, resSalt, 20, DealMode::PerTurn, 10, 10});
    d.items.push_back(DealItem{DealSide::B, kGold, 60, DealMode::PerTurn, 10, 10});
    rules::concludeDeal(tx, d);
    Deal g;
    g.kind = DealKind::Trade;
    g.a = st[0];
    g.b = st[1];
    g.items.push_back(DealItem{DealSide::A, 2, 120, DealMode::Once, 1, 0});
    rules::concludeDeal(tx, g);
  }
  rules::imposeTribute(tx, DealKind::Tribute, st[6], st[4], 45, 8);
  rules::imposeTribute(tx, DealKind::Reparations, st[5], st[2], 30, 5);

  // Время записей хроники — постоянное: мир побайтно одинаков при каждом построении.
  for (Id id : tx.w().log.ids()) {
    LogEntry e = *tx.w().log.get(id);
    e.at = "2026-10-01T00:00:00Z";
    tx.add(std::move(e));
  }
  World out = std::move(tx).finish();
  io::Warnings warns;
  io::normalize(out, warns);
  for (const io::Warning& wn : warns) logWarn("Демонстрационный мир: %s", wn.text().c_str());
  return out;
}

std::mutex gDemoMu;
std::vector<std::pair<std::string, World>> gDemo;

}  // namespace

World makeDemoWorld(const Basemap& basemap) {
  std::lock_guard<std::mutex> lk(gDemoMu);
  const std::string key = basemap.dir() + "|" + basemap.id();
  for (auto& [k, w] : gDemo)
    if (k == key) return w;
  World w = build(basemap);
  gDemo.push_back({key, w});
  return w;
}

void buildDemoWorld(Tx& tx, const Basemap& basemap) {
  const World& cur = tx.w();
  if (!cur.nodes.empty() || !cur.provinces.empty() || !cur.factions.empty() || !cur.armies.empty())
    fail("Демонстрационный мир создаётся только в пустом мире");
  const World d = makeDemoWorld(basemap);
  tx.meta() = *d.meta;
  tx.settings() = *d.settings;
  tx.catalogs() = *d.catalogs;
  tx.relations() = *d.relations;
  d.nodes.each([&](const Node& x) { tx.add(x); });
  d.edges.each([&](const Edge& x) { tx.add(x); });
  d.provinces.each([&](const Province& x) { tx.add(x); });
  d.factions.each([&](const Faction& x) { tx.add(x); });
  d.characters.each([&](const Character& x) { tx.add(x); });
  d.modifiers.each([&](const Modifier& x) { tx.add(x); });
  d.buildings.each([&](const Building& x) { tx.add(x); });
  d.techs.each([&](const Tech& x) { tx.add(x); });
  d.armies.each([&](const Army& x) { tx.add(x); });
  d.routes.each([&](const Route& x) { tx.add(x); });
  d.deals.each([&](const Deal& x) { tx.add(x); });
  d.log.each([&](const LogEntry& x) { tx.add(x); });
  tx.meta().seq = d.meta->seq;
}

void buildDemoWorld(Store& store, const Basemap& basemap) { store.replace(makeDemoWorld(basemap), "Демонстрационный мир", true); }

}  // namespace rg::map
