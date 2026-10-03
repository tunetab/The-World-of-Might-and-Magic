// Regnum — нормализация мира: пределы значений, висячие ссылки, повторы, инварианты схемы.
// Работает на любом World (прочитанном из файлов, исправленном вручную или агентом): после неё
// редактор и правила могут полагаться на целостность ссылок.
#include <set>
#include <tuple>
#include <unordered_set>

#include "core/io_internal.h"

namespace rg::io {

using namespace detail;

namespace {

constexpr int kMaxTurn = 1000000;
constexpr int kMaxTurns = 1000;          // длительность строительства/исследования, ходов
constexpr i64 kMaxCount = 1000000000000000LL;  // численности и население (точно в double)
constexpr int kMinAutosave = 10, kMaxAutosave = 3600;

// Родительный падеж для сообщений «нет провинции p12».
const char* genitive(Seq s) {
  switch (s) {
    case Seq::Province: return "провинции";
    case Seq::Faction: return "фракции";
    case Seq::Character: return "персонажа";
    case Seq::Modifier: return "модификатора";
    case Seq::Building: return "постройки";
    case Seq::Tech: return "технологии";
    case Seq::Army: return "войска";
    case Seq::Route: return "маршрута";
    case Seq::Deal: return "сделки";
    case Seq::Log: return "записи хроники";
    case Seq::Row: return "строки войск";
    case Seq::Council: return "места совета";
    case Seq::Node: return "узла";
    case Seq::Edge: return "дуги";
    case Seq::Resource: return "ресурса";
    case Seq::Race: return "расы";
    case Seq::Culture: return "культуры";
    case Seq::Religion: return "религии";
    case Seq::Government: return "формы правления";
    case Seq::Position: return "должности";
    default: return "объекта";
  }
}

std::string ns(double v) {
  std::string s;
  json::appendNumber(s, v);
  return s;
}
std::string ref(Seq s, Id id) { return id ? refStr(s, id) : std::string("0"); }

bool isPng(const std::string& b) { return b.size() >= 8 && std::memcmp(b.data(), "\x89PNG\r\n\x1a\n", 8) == 0; }
bool isJpeg(const std::string& b) { return b.size() >= 3 && u8(b[0]) == 0xFF && u8(b[1]) == 0xD8 && u8(b[2]) == 0xFF; }

struct Norm {
  Tx& tx;
  Warnings& out;
  std::array<u32, kSeqCount> seq{};
  std::unordered_set<Id> cat[6];  // ресурсы, расы, культуры, религии, формы правления, должности
  std::map<std::tuple<Id, bool, Id>, Id> rowRemap;  // (фракция, флот, прежний ID) -> новый ID строки

  explicit Norm(Tx& t, Warnings& o) : tx(t), out(o) { seq = t.w().meta->seq; }

  const World& w() const { return tx.w(); }
  void warn(FileId f, std::string where, std::string msg) { out.push_back(Warning{kFiles[f].path, std::move(where), std::move(msg)}); }

  // Новый ID последовательности (не меньше уже занятых).
  Id fresh(Seq s, u32 maxInData) {
    u32& c = seq[size_t(s)];
    c = std::max(c, maxInData);
    return ++c;
  }

  bool hasCat(Seq s, Id id) const {
    switch (s) {
      case Seq::Resource: return cat[0].count(id) > 0;
      case Seq::Race: return cat[1].count(id) > 0;
      case Seq::Culture: return cat[2].count(id) > 0;
      case Seq::Religion: return cat[3].count(id) > 0;
      case Seq::Government: return cat[4].count(id) > 0;
      case Seq::Position: return cat[5].count(id) > 0;
      default: return false;
    }
  }
  bool exists(Seq s, Id id) const {
    switch (s) {
      case Seq::Province: return w().provinces.has(id);
      case Seq::Faction: return w().factions.has(id);
      case Seq::Character: return w().characters.has(id);
      case Seq::Modifier: return w().modifiers.has(id);
      case Seq::Building: return w().buildings.has(id);
      case Seq::Tech: return w().techs.has(id);
      case Seq::Army: return w().armies.has(id);
      case Seq::Node: return w().nodes.has(id);
      default: return hasCat(s, id);
    }
  }
  bool isState(Id id) const { const Faction* f = w().faction(id); return f && f->isState(); }
  bool isGuild(Id id) const { const Faction* f = w().faction(id); return f && f->isGuild(); }

  // Одиночная ссылка: несуществующая обнуляется.
  bool fixRef(Id& id, Seq s, FileId f, const std::string& base, const char* field) {
    if (!id || exists(s, id)) return false;
    warn(f, base + "." + field, std::string("нет ") + genitive(s) + " " + refStr(s, id) + " — ссылка удалена");
    id = 0;
    return true;
  }
  // Список ссылок: несуществующие и повторы удаляются.
  template <class Pred>
  bool fixRefList(std::vector<Id>& v, Seq s, FileId f, const std::string& base, const char* field, Pred&& valid,
                  const char* invalidWhy = nullptr) {
    bool bad = false;
    for (size_t i = 0; i < v.size() && !bad; i++) {
      if (!v[i] || !valid(v[i])) bad = true;
      for (size_t k = 0; k < i && !bad; k++) bad = v[k] == v[i];
    }
    if (!bad) return false;
    std::vector<Id> res;
    res.reserve(v.size());
    for (Id id : v) {
      if (!id) continue;
      if (!valid(id)) {
        if (!exists(s, id)) warn(f, base + "." + field, std::string("нет ") + genitive(s) + " " + refStr(s, id) + " — ссылка удалена");
        else warn(f, base + "." + field, refStr(s, id) + ": " + (invalidWhy ? invalidWhy : "недопустимая ссылка") + " — удалена");
        continue;
      }
      if (std::find(res.begin(), res.end(), id) != res.end()) {
        warn(f, base + "." + field, "повтор " + refStr(s, id) + " — удалён");
        continue;
      }
      res.push_back(id);
    }
    v = std::move(res);
    return true;
  }
  bool fixRefList(std::vector<Id>& v, Seq s, FileId f, const std::string& base, const char* field) {
    return fixRefList(v, s, f, base, field, [&](Id id) { return exists(s, id); });
  }

  // Число: конечное и в пределах.
  template <class T>
  bool fixNum(T& v, T lo, T hi, T def, FileId f, const std::string& base, const char* field) {
    if constexpr (std::is_floating_point_v<T>) {
      if (!std::isfinite(v)) {
        warn(f, base + "." + field, "значение не является числом — взято " + ns(double(def)));
        v = def;
        return true;
      }
    }
    if (v < lo || v > hi) {
      T c = clamp(v, lo, hi);
      warn(f, base + "." + field, "значение " + ns(double(v)) + " вне пределов " + ns(double(lo)) + "…" + ns(double(hi)) + " — взято " + ns(double(c)));
      v = c;
      return true;
    }
    return false;
  }
  bool fixFinite(double& v, FileId f, const std::string& base, const char* field) {
    if (std::isfinite(v)) return false;
    warn(f, base + "." + field, "значение не является числом — взят 0");
    v = 0;
    return true;
  }
  template <class E>
  bool fixEnum(E& v, int n, E def, FileId f, const std::string& base, const char* field) {
    if (int(v) >= 0 && int(v) < n) return false;
    warn(f, base + "." + field, "недопустимое значение перечисления — взято по умолчанию");
    v = def;
    return true;
  }
  bool fixPoint(Vec2& p, bool map, FileId f, const std::string& base, const char* field) {
    bool ch = false;
    if (!std::isfinite(p.x) || !std::isfinite(p.y)) {
      warn(f, base + "." + field, "координаты не являются числами — взято [0, 0]");
      p = {};
      ch = true;
    }
    if (map) {
      Vec2 c{clamp(p.x, 0.0, schema::kMapWidth), clamp(p.y, 0.0, schema::kMapHeight)};
      if (c != p) {
        warn(f, base + "." + field, "точка [" + ns(p.x) + ", " + ns(p.y) + "] вне карты — перенесена на край");
        p = c;
        ch = true;
      }
    }
    return ch;
  }
  bool fixPoints(std::vector<Vec2>& pts, FileId f, const std::string& base, const char* field) {
    bool ch = false;
    size_t bad = 0;
    for (const Vec2& p : pts) if (!std::isfinite(p.x) || !std::isfinite(p.y)) bad++;
    if (bad) {
      pts.erase(std::remove_if(pts.begin(), pts.end(), [](const Vec2& p) { return !std::isfinite(p.x) || !std::isfinite(p.y); }), pts.end());
      warn(f, base + "." + field, "точек с нечисловыми координатами: " + std::to_string(bad) + " — удалены");
      ch = true;
    }
    size_t outside = 0;
    for (Vec2& p : pts) {
      Vec2 c{clamp(p.x, 0.0, schema::kMapWidth), clamp(p.y, 0.0, schema::kMapHeight)};
      if (c != p) { p = c; outside++; }
    }
    if (outside) {
      warn(f, base + "." + field, "точек вне карты: " + std::to_string(outside) + " — перенесены на край");
      ch = true;
    }
    return ch;
  }

  // ---------------------------------------------------------------- шаги
  void meta() {
    Meta m = *w().meta;
    bool ch = fixNum(m.turn, 1, kMaxTurn, 1, F_WORLD, "meta", "turn");
    if (m.basemap.empty()) {
      m.basemap = Meta{}.basemap;
      warn(F_WORLD, "meta.basemap", "не задана базовая карта — взята «" + m.basemap + "»");
      ch = true;
    }
    if (ch) tx.meta() = std::move(m);

    Settings s = *w().settings;
    bool sc = false;
    if (!std::isfinite(s.fillOpacity) || s.fillOpacity < 0 || s.fillOpacity > 1) {
      float c = std::isfinite(s.fillOpacity) ? clamp(s.fillOpacity, 0.f, 1.f) : 0.5f;
      warn(F_WORLD, "settings.fillOpacity", "прозрачность заливки " + ns(s.fillOpacity) + " вне пределов 0…1 — взято " + ns(c));
      s.fillOpacity = c;
      sc = true;
    }
    sc |= fixNum(s.autosaveSec, kMinAutosave, kMaxAutosave, 60, F_WORLD, "settings", "autosaveSec");
    sc |= fixEnum(s.occupiedIncome, 3, OccupiedIncome::Owner, F_WORLD, "settings", "occupiedIncome");
    if (sc) tx.settings() = s;
  }

  void catalogs() {
    static const char* const keys[6] = {"resources", "races", "cultures", "religions", "governments", "positions"};
    static const Seq seqs[6] = {Seq::Resource, Seq::Race, Seq::Culture, Seq::Religion, Seq::Government, Seq::Position};
    Catalogs c = *w().catalogs;
    std::vector<CatalogItem>* lists[6] = {&c.resources, &c.races, &c.cultures, &c.religions, &c.governments, &c.positions};
    bool ch = false;
    for (int i = 0; i < 6; i++) {
      auto& list = *lists[i];
      u32 maxId = 0;
      for (auto& it : list) maxId = std::max(maxId, it.id);
      std::unordered_set<Id> seen;
      for (auto& it : list) {
        if (it.id == 0 || seen.count(it.id)) {
          Id old = it.id;
          it.id = fresh(seqs[i], maxId);
          std::string where = std::string(keys[i]) + "." + refStr(seqs[i], it.id);
          warn(F_CATALOGS, where, old ? "повторный ID " + refStr(seqs[i], old) + " — назначен новый" : std::string("запись без ID — назначен новый"));
          ch = true;
        }
        seen.insert(it.id);
      }
    }
    // Встроенный ресурс «Золото» (казна) — всегда rs1.
    auto gold = std::find_if(c.resources.begin(), c.resources.end(), [](const CatalogItem& it) { return it.id == kGold; });
    if (gold == c.resources.end()) {
      CatalogItem g;
      g.id = kGold;
      g.name = "Золото";
      g.color = Color::hex(0xe2b33c);
      g.icon = "coins";
      g.builtin = true;
      c.resources.insert(c.resources.begin(), g);
      warn(F_CATALOGS, "resources", "нет встроенного ресурса «Золото» (rs1, казна) — добавлен");
      ch = true;
    } else if (!gold->builtin) {
      gold->builtin = true;
      warn(F_CATALOGS, "resources.rs1", "rs1 — встроенный ресурс «Золото» (казна): отмечен как встроенный");
      ch = true;
    }
    for (int i = 0; i < 6; i++)
      for (auto& it : *lists[i]) cat[i].insert(it.id);
    if (ch) tx.catalogs() = std::move(c);
  }

  void nodes() {
    const World base = w();
    base.nodes.each([&](const Node& n) {
      if (!std::isfinite(n.p.x) || !std::isfinite(n.p.y)) {
        warn(F_GEO, "nodes." + std::to_string(n.id), "координаты узла не являются числами — узел удалён");
        tx.eraseNode(n.id);
        return;
      }
      Vec2 p = n.p;
      if (fixPoint(p, true, F_GEO, "nodes." + std::to_string(n.id), "p")) tx.node(n.id).p = p;
    });
  }

  void edges() {
    const World base = w();
    base.edges.each([&](const Edge& e0) {
      std::string where = "edges." + std::to_string(e0.id);
      if (!exists(Seq::Node, e0.a) || !exists(Seq::Node, e0.b)) {
        warn(F_GEO, where, "дуга ссылается на несуществующий узел " + std::to_string(exists(Seq::Node, e0.a) ? e0.b : e0.a) + " — дуга удалена");
        tx.eraseEdge(e0.id);
        return;
      }
      Edge e = e0;
      bool ch = fixPoints(e.pts, F_GEO, where, "pts");
      if (e.a == e.b && e.pts.size() < 2) {
        warn(F_GEO, where, "замкнутая дуга без промежуточных точек — дуга удалена");
        tx.eraseEdge(e.id);
        return;
      }
      ch |= fixEnum(e.kind, 3, EdgeKind::Border, F_GEO, where, "kind");
      ch |= fixEnum(e.tl, 3, Terrain::Land, F_GEO, where, "tl");
      ch |= fixEnum(e.tr, 3, Terrain::Land, F_GEO, where, "tr");
      ch |= fixRef(e.pl, Seq::Province, F_GEO, where, "pl");
      ch |= fixRef(e.pr, Seq::Province, F_GEO, where, "pr");
      if (ch) tx.edge(e.id) = std::move(e);
    });
  }

  void factions() {
    const World base = w();
    u32 maxRow = 0, maxSeat = 0;
    base.factions.each([&](const Faction& f) {
      for (auto& r : f.army) maxRow = std::max(maxRow, r.id);
      for (auto& r : f.fleet) maxRow = std::max(maxRow, r.id);
      for (auto& s : f.council) maxSeat = std::max(maxSeat, s.id);
    });
    std::unordered_set<Id> usedRows, usedSeats;
    base.factions.each([&](const Faction& f0) {
      Faction f = f0;
      const std::string where = refStr(Seq::Faction, f.id);
      bool ch = fixEnum(f.kind, 2, FactionKind::State, F_FACTIONS, where, "kind");

      // Строки войск и флота: ID уникальны во всём мире (общая последовательность u).
      auto rows = [&](auto& list, bool fleet, const char* field, int types) {
        std::unordered_set<Id> local;
        for (size_t i = 0; i < list.size(); i++) {
          auto& r = list[i];
          std::string at = where + "." + field + "[" + std::to_string(i) + "]";
          Id old = r.id;
          if (old == 0) {
            r.id = fresh(Seq::Row, maxRow);
            warn(F_FACTIONS, at, "строка без ID — назначен " + refStr(Seq::Row, r.id));
            ch = true;
          } else if (local.count(old)) {
            r.id = fresh(Seq::Row, maxRow);
            warn(F_FACTIONS, at, "повторный ID " + refStr(Seq::Row, old) + " в списке — назначен " + refStr(Seq::Row, r.id) + " (ссылки остаются на первую строку)");
            ch = true;
          } else if (usedRows.count(old)) {
            r.id = fresh(Seq::Row, maxRow);
            rowRemap[{f.id, fleet, old}] = r.id;
            warn(F_FACTIONS, at, "ID " + refStr(Seq::Row, old) + " уже занят другой строкой — назначен " + refStr(Seq::Row, r.id) + ", ссылки обновлены");
            ch = true;
          }
          local.insert(r.id);
          usedRows.insert(r.id);
          if (int(r.type) < 0 || int(r.type) >= types) {
            warn(F_FACTIONS, at + ".type", "недопустимый тип — взят по умолчанию");
            r.type = decltype(r.type)(fleet ? int(ShipType::Frigate) : 0);
            ch = true;
          }
          ch |= fixNum(r.total, i64(0), kMaxCount, i64(0), F_FACTIONS, at, "total");
          ch |= fixNum(r.upkeep, 0.0, 1e12, 0.0, F_FACTIONS, at, "upkeep");
        }
      };
      rows(f.army, false, "army", int(UnitType::Count));
      rows(f.fleet, true, "fleet", int(ShipType::Count));

      for (size_t i = 0; i < f.council.size(); i++) {
        auto& s = f.council[i];
        std::string at = where + ".council[" + std::to_string(i) + "]";
        if (s.id == 0 || usedSeats.count(s.id)) {
          Id old = s.id;
          s.id = fresh(Seq::Council, maxSeat);
          warn(F_FACTIONS, at, old ? "повторный ID " + refStr(Seq::Council, old) + " — назначен " + refStr(Seq::Council, s.id)
                                   : "место без ID — назначен " + refStr(Seq::Council, s.id));
          ch = true;
        }
        usedSeats.insert(s.id);
        ch |= fixRef(s.character, Seq::Character, F_FACTIONS, at, "character");
      }

      if (f.flag.pattern >= FlagPattern::Count) {
        warn(F_FACTIONS, where + ".flag.pattern", "недопустимый узор — взят однотонный");
        f.flag.pattern = FlagPattern::Solid;
        ch = true;
      }
      if (!f.flag.png.empty() && !isPng(f.flag.png)) {
        warn(F_FACTIONS, where + ".flag.png", "данные флага не являются PNG — изображение удалено");
        f.flag.png.clear();
        f.flag.image = false;
        ch = true;
      }
      if (f.flag.image && f.flag.png.empty()) {
        warn(F_FACTIONS, where + ".flag.image", "флаг-изображение без PNG — включён конструктор");
        f.flag.image = false;
        ch = true;
      }

      ch |= fixRef(f.culture, Seq::Culture, F_FACTIONS, where, "culture");
      ch |= fixRef(f.government, Seq::Government, F_FACTIONS, where, "government");
      ch |= fixRef(f.religion, Seq::Religion, F_FACTIONS, where, "religion");
      ch |= fixRef(f.ruler, Seq::Character, F_FACTIONS, where, "ruler");
      ch |= fixRef(f.capital, Seq::Province, F_FACTIONS, where, "capital");

      for (auto it = f.res.begin(); it != f.res.end();) {
        if (!hasCat(Seq::Resource, it->first)) {
          warn(F_FACTIONS, where + ".res", "нет ресурса " + refStr(Seq::Resource, it->first) + " — запас удалён");
          it = f.res.erase(it);
          ch = true;
        } else {
          ch |= fixFinite(it->second, F_FACTIONS, where + ".res", refStr(Seq::Resource, it->first).c_str());
          ++it;
        }
      }
      ch |= fixRefList(f.modifiers, Seq::Modifier, F_FACTIONS, where, "modifiers");
      ch |= fixNum(f.tax, 0.0, 100.0, 10.0, F_FACTIONS, where, "tax");

      if (f.isState()) {
        if (f.homeState) {
          warn(F_FACTIONS, where + ".homeState", "у государства не бывает государства расположения — удалено");
          f.homeState = 0;
          ch = true;
        }
        if (f.stateGuild) {
          warn(F_FACTIONS, where + ".stateGuild", "признак государственной гильдии у государства — снят");
          f.stateGuild = false;
          ch = true;
        }
      } else if (f.homeState && !isState(f.homeState)) {
        warn(F_FACTIONS, where + ".homeState", exists(Seq::Faction, f.homeState) ? refStr(Seq::Faction, f.homeState) + " — не государство: ссылка удалена"
                                                                               : "нет фракции " + refStr(Seq::Faction, f.homeState) + " — ссылка удалена");
        f.homeState = 0;
        ch = true;
      }
      if (ch) tx.faction(f.id) = std::move(f);
    });
  }

  void characters() {
    const World base = w();
    base.characters.each([&](const Character& c0) {
      Character c = c0;
      const std::string where = refStr(Seq::Character, c.id);
      bool ch = fixRef(c.faction, Seq::Faction, F_CHARACTERS, where, "faction");
      ch |= fixNum(c.upkeep, 0.0, 1e12, 0.0, F_CHARACTERS, where, "upkeep");
      if (!c.portrait.empty() && !isPng(c.portrait) && !isJpeg(c.portrait)) {
        warn(F_CHARACTERS, where + ".portrait", "портрет не является PNG или JPEG — удалён");
        c.portrait.clear();
        ch = true;
      }
      if (ch) tx.character(c.id) = std::move(c);
    });
  }

  void modifiers() {
    const World base = w();
    base.modifiers.each([&](const Modifier& m0) {
      Modifier m = m0;
      const std::string where = refStr(Seq::Modifier, m.id);
      bool ch = false;
      u32 valid = (1u << kFxCount) - 1;
      if (m.fxMask & ~valid) {
        m.fxMask &= valid;
        ch = true;
      }
      for (int i = 0; i < kFxCount; i++) {
        const auto& e = schema::kEffects[i];
        double& v = m.fx[size_t(i)];
        if (!m.has(Fx(i))) {
          if (v != 0) { v = 0; ch = true; }
          continue;
        }
        if (!std::isfinite(v)) {
          warn(F_MODIFIERS, where + ".fx." + e.id, "значение эффекта не является числом — эффект удалён");
          m.fxMask &= ~(1u << i);
          v = 0;
          ch = true;
          continue;
        }
        ch |= fixNum(v, e.min, e.max, 0.0, F_MODIFIERS, where + ".fx", e.id);
      }
      ch |= fixRefList(m.targets, Seq::Faction, F_MODIFIERS, where, "targets");
      if (ch) tx.modifier(m.id) = std::move(m);
    });
  }

  // Разорвать циклы графа требований: deps(id) — список зависимостей; удаляются обратные рёбра (обход по возрастанию ID).
  template <class GetDeps>
  std::set<std::pair<Id, Id>> cycleEdges(const std::vector<Id>& ids, GetDeps&& deps) {
    std::set<std::pair<Id, Id>> cut;
    std::unordered_map<Id, u8> color;  // 0 — белый, 1 — в стеке, 2 — готово
    for (Id start : ids) {
      if (color[start]) continue;
      std::vector<std::pair<Id, size_t>> stack{{start, 0}};
      color[start] = 1;
      while (!stack.empty()) {
        auto& [id, i] = stack.back();
        const std::vector<Id>& d = deps(id);
        if (i >= d.size()) {
          color[id] = 2;
          stack.pop_back();
          continue;
        }
        Id next = d[i++];
        u8& c = color[next];
        if (c == 1) cut.insert({id, next});
        else if (c == 0) {
          c = 1;
          stack.push_back({next, 0});
        }
      }
    }
    return cut;
  }

  void buildings() {
    {
      const World base = w();
      base.buildings.each([&](const Building& b0) {
        Building b = b0;
        const std::string where = refStr(Seq::Building, b.id);
        bool ch = false;
        if (b.owner && !exists(Seq::Faction, b.owner)) {
          warn(F_BUILDINGS, where + ".owner", "нет фракции " + refStr(Seq::Faction, b.owner) + " — постройка стала общей");
          b.owner = 0;
          ch = true;
        } else if (b.owner && !isState(b.owner)) {
          // Уникальные постройки бывают только у государств: гильдия не владеет провинциями.
          warn(F_BUILDINGS, where + ".owner", refStr(Seq::Faction, b.owner) + " — гильдия, а уникальная постройка бывает только у государства: постройка стала общей");
          b.owner = 0;
          ch = true;
        }
        ch |= fixEnum(b.cat, int(BuildingCat::Count), BuildingCat::Economic, F_BUILDINGS, where, "cat");
        if (b.levels.empty()) {
          warn(F_BUILDINGS, where + ".levels", "у постройки нет уровней — добавлен один");
          b.levels.push_back(BuildingLevel{});
          ch = true;
        }
        for (size_t i = 0; i < b.levels.size(); i++) {
          auto& l = b.levels[i];
          std::string at = where + ".levels[" + std::to_string(i) + "]";
          ch |= fixNum(l.turns, 1, kMaxTurns, 1, F_BUILDINGS, at, "turns");
          for (auto it = l.cost.begin(); it != l.cost.end();) {
            if (!hasCat(Seq::Resource, it->first)) {
              warn(F_BUILDINGS, at + ".cost", "нет ресурса " + refStr(Seq::Resource, it->first) + " — цена удалена");
              it = l.cost.erase(it);
              ch = true;
            } else {
              ch |= fixNum(it->second, 0.0, 1e12, 0.0, F_BUILDINGS, at + ".cost", refStr(Seq::Resource, it->first).c_str());
              ++it;
            }
          }
          ch |= fixRefList(l.modifiers, Seq::Modifier, F_BUILDINGS, at, "modifiers");
        }
        ch |= fixPoint(b.pos, false, F_BUILDINGS, where, "pos");
        if (ch) tx.building(b.id) = std::move(b);
      });
    }
    // Требования: существующая постройка, не сама, без повторов, уровень в пределах.
    {
      const World base = w();
      base.buildings.each([&](const Building& b0) {
        Building b = b0;
        const std::string where = refStr(Seq::Building, b.id);
        bool ch = false;
        std::vector<BuildingReq> res;
        for (auto& r : b.requires_) {
          const Building* t = base.building(r.building);
          if (!t) {
            warn(F_BUILDINGS, where + ".requires", "нет постройки " + ref(Seq::Building, r.building) + " — требование удалено");
            ch = true;
            continue;
          }
          if (r.building == b.id) {
            warn(F_BUILDINGS, where + ".requires", "постройка требует саму себя — требование удалено");
            ch = true;
            continue;
          }
          if (std::any_of(res.begin(), res.end(), [&](const BuildingReq& q) { return q.building == r.building; })) {
            warn(F_BUILDINGS, where + ".requires", "повтор требования " + refStr(Seq::Building, r.building) + " — удалён");
            ch = true;
            continue;
          }
          BuildingReq q = r;
          ch |= fixNum(q.level, 1, int(t->levels.size()), 1, F_BUILDINGS, where + ".requires." + refStr(Seq::Building, r.building), "level");
          res.push_back(q);
        }
        if (ch) {
          b.requires_ = std::move(res);
          tx.building(b.id) = std::move(b);
        }
      });
    }
    // Циклы требований.
    const World base = w();
    std::unordered_map<Id, std::vector<Id>> deps;
    base.buildings.each([&](const Building& b) {
      auto& d = deps[b.id];
      for (auto& r : b.requires_) d.push_back(r.building);
    });
    auto cut = cycleEdges(base.buildings.ids(), [&](Id id) -> const std::vector<Id>& { return deps[id]; });
    for (auto& [from, to] : cut) {
      warn(F_BUILDINGS, refStr(Seq::Building, from) + ".requires", "требование " + refStr(Seq::Building, to) + " замыкает цикл — удалено");
      auto& rq = tx.building(from).requires_;
      Id target = to;
      rq.erase(std::remove_if(rq.begin(), rq.end(), [&](const BuildingReq& q) { return q.building == target; }), rq.end());
    }
  }

  void techs() {
    {
      const World base = w();
      base.techs.each([&](const Tech& t) {
        if (!exists(Seq::Faction, t.faction)) {
          warn(F_TECHS, refStr(Seq::Tech, t.id), t.faction ? "нет фракции " + refStr(Seq::Faction, t.faction) + " — технология удалена"
                                                            : std::string("технология без фракции — удалена"));
          tx.eraseTech(t.id);
        }
      });
    }
    {
      const World base = w();
      base.techs.each([&](const Tech& t0) {
        Tech t = t0;
        const std::string where = refStr(Seq::Tech, t.id);
        bool ch = fixNum(t.turns, 1, kMaxTurns, 1, F_TECHS, where, "turns");
        ch |= fixNum(t.progress, 0, t.turns, 0, F_TECHS, where, "progress");
        if (t.studied && t.research) {
          warn(F_TECHS, where + ".research", "изученная технология не может исследоваться — исследование снято");
          t.research = false;
          ch = true;
        }
        ch |= fixRefList(t.prereqs, Seq::Tech, F_TECHS, where, "prereqs",
                         [&](Id id) { const Tech* p = base.tech(id); return p && id != t.id && p->faction == t.faction; },
                         "технология другой фракции или она сама");
        ch |= fixRefList(t.modifiers, Seq::Modifier, F_TECHS, where, "modifiers");
        ch |= fixPoint(t.pos, false, F_TECHS, where, "pos");
        if (ch) tx.tech(t.id) = std::move(t);
      });
    }
    const World base = w();
    auto ids = base.techs.ids();
    auto cut = cycleEdges(ids, [&](Id id) -> const std::vector<Id>& { return base.tech(id)->prereqs; });
    for (auto& [from, to] : cut) {
      warn(F_TECHS, refStr(Seq::Tech, from) + ".prereqs", "условие " + refStr(Seq::Tech, to) + " замыкает цикл — удалено");
      auto& pr = tx.tech(from).prereqs;
      pr.erase(std::remove(pr.begin(), pr.end(), to), pr.end());
    }
    // ТЗ 1.b.v: изученная технология требует изученных предшествующих. Иначе изученность снимается (по цепочке —
    // и у зависящих от неё); пройденные ходы сохраняются.
    for (bool again = true; again;) {
      again = false;
      const World cur = w();
      cur.techs.each([&](const Tech& t) {
        if (!t.studied) return;
        for (Id pid : t.prereqs) {
          const Tech* pt = cur.tech(pid);
          if (!pt || pt->studied) continue;
          warn(F_TECHS, refStr(Seq::Tech, t.id) + ".studied",
               "не изучено условие " + refStr(Seq::Tech, pid) + " — изученность снята");
          Tech& m = tx.tech(t.id);
          m.studied = false;
          m.research = false;
          m.progress = std::min(m.progress, std::max(0, m.turns - 1));
          again = true;
          return;
        }
      });
    }
  }

  Id remapRow(Id faction, bool fleet, Id row) const {
    auto it = rowRemap.find({faction, fleet, row});
    return it == rowRemap.end() ? row : it->second;
  }

  void provinces() {
    const World base = w();
    base.provinces.each([&](const Province& p0) {
      Province p = p0;
      const std::string where = refStr(Seq::Province, p.id);
      bool ch = false;
      if (p.owner && !isState(p.owner)) {
        warn(F_PROVINCES, where + ".owner", exists(Seq::Faction, p.owner) ? refStr(Seq::Faction, p.owner) + " — гильдия, а владельцем может быть только государство: владелец удалён"
                                                                         : "нет фракции " + refStr(Seq::Faction, p.owner) + " — владелец удалён");
        p.owner = 0;
        ch = true;
      }
      ch |= fixRef(p.lord, Seq::Character, F_PROVINCES, where, "lord");
      ch |= fixEnum(p.size, 3, ProvSize::Medium, F_PROVINCES, where, "size");
      ch |= fixEnum(p.city, 4, CityType::Village, F_PROVINCES, where, "city");
      ch |= fixRef(p.resource, Seq::Resource, F_PROVINCES, where, "resource");
      ch |= fixNum(p.resourceAmount, 0.0, 1e12, 0.0, F_PROVINCES, where, "resourceAmount");

      // Гарнизон: строки армии владельца, без повторов (численности складываются).
      if (!p.garrison.empty()) {
        const Faction* owner = base.faction(p.owner);
        std::vector<GarrisonEntry> res;
        bool gch = false;
        for (size_t i = 0; i < p.garrison.size(); i++) {
          GarrisonEntry g = p.garrison[i];
          std::string at = where + ".garrison[" + std::to_string(i) + "]";
          Id row = owner ? remapRow(owner->id, false, g.row) : g.row;
          if (row != g.row) { g.row = row; gch = true; }
          if (!owner) {
            warn(F_PROVINCES, at, "у провинции нет владельца — гарнизон удалён");
            gch = true;
            continue;
          }
          if (!owner->armyRow(g.row)) {
            warn(F_PROVINCES, at + ".row", "у владельца " + refStr(Seq::Faction, owner->id) + " нет строки армии " + ref(Seq::Row, g.row) + " — запись удалена");
            gch = true;
            continue;
          }
          gch |= fixNum(g.count, i64(0), kMaxCount, i64(0), F_PROVINCES, at, "count");
          auto same = std::find_if(res.begin(), res.end(), [&](const GarrisonEntry& x) { return x.row == g.row; });
          if (same != res.end()) {
            warn(F_PROVINCES, at, "повтор строки " + refStr(Seq::Row, g.row) + " — численности сложены");
            same->count = std::min(kMaxCount, same->count + g.count);
            gch = true;
            continue;
          }
          res.push_back(g);
        }
        if (gch) { p.garrison = std::move(res); ch = true; }
      }

      ch |= fixNum(p.contentment, -100.0, 100.0, 0.0, F_PROVINCES, where, "contentment");
      ch |= fixRef(p.culture, Seq::Culture, F_PROVINCES, where, "culture");
      ch |= fixRef(p.religion, Seq::Religion, F_PROVINCES, where, "religion");

      if (!p.races.empty()) {
        std::vector<RacePop> res;
        bool rch = false;
        for (size_t i = 0; i < p.races.size(); i++) {
          RacePop r = p.races[i];
          std::string at = where + ".races[" + std::to_string(i) + "]";
          if (!hasCat(Seq::Race, r.race)) {
            warn(F_PROVINCES, at + ".race", "нет расы " + ref(Seq::Race, r.race) + " — запись удалена");
            rch = true;
            continue;
          }
          rch |= fixNum(r.pop, i64(0), kMaxCount, i64(0), F_PROVINCES, at, "pop");
          auto same = std::find_if(res.begin(), res.end(), [&](const RacePop& x) { return x.race == r.race; });
          if (same != res.end()) {
            warn(F_PROVINCES, at, "повтор расы " + refStr(Seq::Race, r.race) + " — население сложено");
            same->pop = std::min(kMaxCount, same->pop + r.pop);
            rch = true;
            continue;
          }
          res.push_back(r);
        }
        if (rch) { p.races = std::move(res); ch = true; }
      }

      // Влияние гильдий: только гильдии, без повторов, 0…100, сумма ≤ 100.
      if (!p.influence.empty()) {
        std::vector<Influence> res;
        bool ich = false;
        for (size_t i = 0; i < p.influence.size(); i++) {
          Influence x = p.influence[i];
          std::string at = where + ".influence[" + std::to_string(i) + "]";
          if (!isGuild(x.guild)) {
            warn(F_PROVINCES, at + ".guild", exists(Seq::Faction, x.guild) ? refStr(Seq::Faction, x.guild) + " — не гильдия: запись удалена"
                                                                          : "нет гильдии " + ref(Seq::Faction, x.guild) + " — запись удалена");
            ich = true;
            continue;
          }
          ich |= fixNum(x.pct, 0.0, 100.0, 0.0, F_PROVINCES, at, "pct");
          auto same = std::find_if(res.begin(), res.end(), [&](const Influence& y) { return y.guild == x.guild; });
          if (same != res.end()) {
            warn(F_PROVINCES, at, "повтор гильдии " + refStr(Seq::Faction, x.guild) + " — влияние сложено");
            same->pct = std::min(100.0, same->pct + x.pct);
            ich = true;
            continue;
          }
          res.push_back(x);
        }
        double sum = 0;
        for (auto& x : res) sum += x.pct;
        if (sum > 100.0 + 1e-9) {
          double k = 100.0 / sum;
          for (auto& x : res) x.pct *= k;
          warn(F_PROVINCES, where + ".influence", "сумма влияния " + ns(sum) + " % больше 100 % — доли уменьшены пропорционально");
          ich = true;
        }
        if (ich) { p.influence = std::move(res); ch = true; }
      }

      // Штабы: только гильдии, без повторов, не больше kMaxHqPerProvince.
      ch |= fixRefList(p.hqs, Seq::Faction, F_PROVINCES, where, "hqs", [&](Id id) { return isGuild(id); }, "не гильдия");
      if (int(p.hqs.size()) > schema::kMaxHqPerProvince) {
        warn(F_PROVINCES, where + ".hqs", "штабов " + std::to_string(p.hqs.size()) + ", допустимо не больше " +
                                              std::to_string(schema::kMaxHqPerProvince) + " — лишние удалены");
        p.hqs.resize(size_t(schema::kMaxHqPerProvince));
        ch = true;
      }

      ch |= fixNum(p.baseTrade, 0.0, 1e12, 0.0, F_PROVINCES, where, "baseTrade");
      ch |= fixNum(p.localTax, -100.0, 100.0, 0.0, F_PROVINCES, where, "localTax");

      if (!p.buildings.empty()) {
        std::vector<ProvBuilding> res;
        bool bch = false;
        for (size_t i = 0; i < p.buildings.size(); i++) {
          ProvBuilding b = p.buildings[i];
          std::string at = where + ".buildings[" + std::to_string(i) + "]";
          const Building* def = base.building(b.building);
          if (!def) {
            warn(F_PROVINCES, at + ".building", "нет постройки " + ref(Seq::Building, b.building) + " — запись удалена");
            bch = true;
            continue;
          }
          if (std::any_of(res.begin(), res.end(), [&](const ProvBuilding& x) { return x.building == b.building; })) {
            warn(F_PROVINCES, at, "повтор постройки " + refStr(Seq::Building, b.building) + " — удалён");
            bch = true;
            continue;
          }
          bch |= fixNum(b.level, 1, int(def->levels.size()), 1, F_PROVINCES, at, "level");
          bch |= fixNum(b.left, 0, kMaxTurns, 0, F_PROVINCES, at, "left");
          if (!b.constructing && b.left != 0) {
            b.left = 0;
            bch = true;
          }
          // Уплаченное за строящийся уровень: только у строящейся постройки; ресурсы каталога, числа ≥ 0;
          // плательщик — существующее государство (иначе возвращать некому).
          if (!b.constructing) {
            if (!b.paid.empty() || b.payer) {
              warn(F_PROVINCES, at + ".paid", "уплаченное указано у достроенной постройки — удалено");
              b.paid.clear();
              b.payer = 0;
              bch = true;
            }
          } else {
            for (auto it = b.paid.begin(); it != b.paid.end();) {
              if (!hasCat(Seq::Resource, it->first)) {
                warn(F_PROVINCES, at + ".paid", "нет ресурса " + refStr(Seq::Resource, it->first) + " — позиция удалена");
                it = b.paid.erase(it);
                bch = true;
              } else {
                bch |= fixNum(it->second, 0.0, 1e12, 0.0, F_PROVINCES, at + ".paid", refStr(Seq::Resource, it->first).c_str());
                ++it;
              }
            }
            if (b.payer && !isState(b.payer)) {
              warn(F_PROVINCES, at + ".payer", exists(Seq::Faction, b.payer) ? refStr(Seq::Faction, b.payer) + " — не государство: плательщик снят"
                                                                            : "нет фракции " + refStr(Seq::Faction, b.payer) + " — плательщик снят");
              b.payer = 0;
              bch = true;
            }
          }
          res.push_back(b);
        }
        if (bch) { p.buildings = std::move(res); ch = true; }
      }
      ch |= fixRefList(p.modifiers, Seq::Modifier, F_PROVINCES, where, "modifiers");

      if (p.occupied) {
        if (!isState(p.occupier) || p.occupier == p.owner) {
          warn(F_PROVINCES, where + ".occupier", p.occupier == 0 ? std::string("оккупация без оккупанта — снята")
                                                 : p.occupier == p.owner ? "оккупант совпадает с владельцем — оккупация снята"
                                                 : exists(Seq::Faction, p.occupier) ? refStr(Seq::Faction, p.occupier) + " — не государство: оккупация снята"
                                                                                    : "нет фракции " + refStr(Seq::Faction, p.occupier) + " — оккупация снята");
          p.occupied = false;
          p.occupier = 0;
          ch = true;
        }
      } else if (p.occupier) {
        warn(F_PROVINCES, where + ".occupier", "оккупант указан, но провинция не оккупирована — оккупант удалён");
        p.occupier = 0;
        ch = true;
      }
      if (ch) tx.province(p.id) = std::move(p);
    });
  }

  void armies() {
    const World base = w();
    std::unordered_set<Id> usedHeroes;
    base.armies.each([&](const Army& a0) {
      Army a = a0;
      const std::string where = refStr(Seq::Army, a.id);
      bool ch = fixEnum(a.kind, 2, ArmyKind::Army, F_ARMIES, where, "kind");
      ch |= fixPoint(a.pos, true, F_ARMIES, where, "pos");
      bool fleet = a.isFleet();
      std::vector<ArmyGroup> groups;
      for (size_t gi = 0; gi < a.groups.size(); gi++) {
        ArmyGroup g = a.groups[gi];
        std::string at = where + ".groups[" + std::to_string(gi) + "]";
        const Faction* f = base.faction(g.faction);
        if (!f) {
          warn(F_ARMIES, at + ".faction", "нет фракции " + ref(Seq::Faction, g.faction) + " — отряды группы удалены");
          ch = true;
          continue;
        }
        std::vector<ArmyUnit> units;
        for (size_t ui = 0; ui < g.units.size(); ui++) {
          ArmyUnit u = g.units[ui];
          std::string uat = at + ".units[" + std::to_string(ui) + "]";
          Id row = remapRow(f->id, fleet, u.row);
          if (row != u.row) { u.row = row; ch = true; }
          if (fleet ? !f->fleetRow(u.row) : !f->armyRow(u.row)) {
            warn(F_ARMIES, uat + ".row", std::string("у ") + refStr(Seq::Faction, f->id) + (fleet ? " нет строки флота " : " нет строки армии ") +
                                             ref(Seq::Row, u.row) + " — отряд удалён");
            ch = true;
            continue;
          }
          ch |= fixNum(u.count, i64(0), kMaxCount, i64(0), F_ARMIES, uat, "count");
          auto same = std::find_if(units.begin(), units.end(), [&](const ArmyUnit& x) { return x.row == u.row; });
          if (same != units.end()) {
            warn(F_ARMIES, uat, "повтор строки " + refStr(Seq::Row, u.row) + " — численности сложены");
            same->count = std::min(kMaxCount, same->count + u.count);
            ch = true;
            continue;
          }
          units.push_back(u);
        }
        g.units = std::move(units);
        // Герой — персонаж, состоящий не больше чем в одном войске.
        std::vector<Id> heroes;
        for (Id h : g.heroes) {
          if (!exists(Seq::Character, h)) {
            warn(F_ARMIES, at + ".heroes", "нет персонажа " + ref(Seq::Character, h) + " — удалён из войска");
            ch = true;
            continue;
          }
          if (usedHeroes.count(h)) {
            warn(F_ARMIES, at + ".heroes", "персонаж " + refStr(Seq::Character, h) + " уже в другом войске или группе — удалён");
            ch = true;
            continue;
          }
          usedHeroes.insert(h);
          heroes.push_back(h);
        }
        g.heroes = std::move(heroes);
        auto same = std::find_if(groups.begin(), groups.end(), [&](const ArmyGroup& x) { return x.faction == g.faction; });
        if (same != groups.end()) {
          warn(F_ARMIES, at, "вторая группа фракции " + refStr(Seq::Faction, g.faction) + " — объединена с первой");
          for (auto& u : g.units) {
            auto su = std::find_if(same->units.begin(), same->units.end(), [&](const ArmyUnit& x) { return x.row == u.row; });
            if (su != same->units.end()) su->count = std::min(kMaxCount, su->count + u.count);
            else same->units.push_back(u);
          }
          for (Id h : g.heroes) same->heroes.push_back(h);
          ch = true;
          continue;
        }
        groups.push_back(std::move(g));
      }
      if (groups.empty()) {
        warn(F_ARMIES, where, a.groups.empty() ? std::string("в войске нет ни одной группы отрядов — войско удалено")
                                               : std::string("не осталось ни одной допустимой группы — войско удалено"));
        tx.eraseArmy(a.id);
        return;
      }
      a.groups = std::move(groups);
      ch |= fixRef(a.commander, Seq::Character, F_ARMIES, where, "commander");
      if (ch) tx.army(a.id) = std::move(a);
    });
  }

  void routes() {
    const World base = w();
    base.routes.each([&](const Route& r0) {
      Route r = r0;
      const std::string where = refStr(Seq::Route, r.id);
      bool ch = false;
      if (r.guild && !isGuild(r.guild)) {
        warn(F_ROUTES, where + ".guild", exists(Seq::Faction, r.guild) ? refStr(Seq::Faction, r.guild) + " — не гильдия: владелец удалён"
                                                                      : "нет фракции " + refStr(Seq::Faction, r.guild) + " — владелец удалён");
        r.guild = 0;
        ch = true;
      }
      ch |= fixPoints(r.pts, F_ROUTES, where, "pts");
      if (r.pts.size() < 2) {
        warn(F_ROUTES, where, "у маршрута меньше двух точек — маршрут удалён");
        tx.eraseRoute(r.id);
        return;
      }
      if (ch) tx.route(r.id) = std::move(r);
    });
  }

  void deals() {
    const World base = w();
    base.deals.each([&](const Deal& d0) {
      Deal d = d0;
      const std::string where = refStr(Seq::Deal, d.id);
      if (!exists(Seq::Faction, d.a) || !exists(Seq::Faction, d.b) || d.a == d.b) {
        warn(F_DEALS, where, d.a == d.b && d.a ? "стороны сделки совпадают — сделка удалена"
                                               : "нет стороны сделки " + ref(Seq::Faction, exists(Seq::Faction, d.a) ? d.b : d.a) + " — сделка удалена");
        tx.eraseDeal(d.id);
        return;
      }
      bool ch = fixEnum(d.kind, 3, DealKind::Trade, F_DEALS, where, "kind");
      ch |= fixEnum(d.status, 3, DealStatus::Active, F_DEALS, where, "status");
      ch |= fixNum(d.turn, 1, kMaxTurn, 1, F_DEALS, where, "turn");
      std::vector<DealItem> items;
      for (size_t i = 0; i < d.items.size(); i++) {
        DealItem it = d.items[i];
        std::string at = where + ".items[" + std::to_string(i) + "]";
        if (!hasCat(Seq::Resource, it.res)) {
          warn(F_DEALS, at + ".res", "нет ресурса " + ref(Seq::Resource, it.res) + " — позиция удалена");
          ch = true;
          continue;
        }
        ch |= fixEnum(it.from, 2, DealSide::A, F_DEALS, at, "from");
        ch |= fixEnum(it.mode, 2, DealMode::Once, F_DEALS, at, "mode");
        ch |= fixNum(it.amount, 0.0, 1e12, 0.0, F_DEALS, at, "amount");
        ch |= fixNum(it.turns, 1, kMaxTurn, 1, F_DEALS, at, "turns");
        ch |= fixNum(it.left, 0, it.turns, 0, F_DEALS, at, "left");
        items.push_back(it);
      }
      if (ch) {
        d.items = std::move(items);
        tx.deal(d.id) = std::move(d);
      }
    });
  }

  // Хроника хранит историю: ссылки на удалённые провинции, войска и фракции допустимы и не проверяются.
  void log() {
    const World base = w();
    base.log.each([&](const LogEntry& l0) {
      const std::string where = refStr(Seq::Log, l0.id);
      LogEntry l = l0;
      bool ch = fixNum(l.turn, 1, kMaxTurn, 1, F_LOG, where, "turn");
      ch |= fixEnum(l.kind, int(LogKind::Count), LogKind::Note, F_LOG, where, "kind");
      ch |= fixRefList(l.factions, Seq::Faction, F_LOG, where, "factions", [](Id) { return true; });
      if (ch) tx.add(std::move(l));  // запись с тем же ID заменяется
    });
  }

  void relations() {
    const RelMap& rel = *w().relations;
    bool bad = false;
    for (auto& [k, r] : rel) {
      Id a = Id(k >> 32), b = Id(k & 0xFFFFFFFFu);
      if (!exists(Seq::Faction, a) || !exists(Seq::Faction, b) || a == b || a > b || !std::isfinite(r.v) || r.v < -100 ||
          r.v > 100 || int(r.s) < 0 || int(r.s) > 3)
        bad = true;
    }
    if (!bad) return;
    RelMap res;
    for (auto& [k, r0] : rel) {
      Id a = Id(k >> 32), b = Id(k & 0xFFFFFFFFu);
      std::string where = refStr(Seq::Faction, std::min(a, b)) + "|" + refStr(Seq::Faction, std::max(a, b));
      if (a == b) {
        warn(F_RELATIONS, where, "отношение фракции к самой себе — удалено");
        continue;
      }
      if (!exists(Seq::Faction, a) || !exists(Seq::Faction, b)) {
        warn(F_RELATIONS, where, "нет фракции " + refStr(Seq::Faction, exists(Seq::Faction, a) ? b : a) + " — отношение удалено");
        continue;
      }
      Relation r = r0;
      fixNum(r.v, -100.0, 100.0, 0.0, F_RELATIONS, where, "v");
      fixEnum(r.s, 4, RelStatus::Unknown, F_RELATIONS, where, "s");
      res[relKey(a, b)] = r;
    }
    tx.relations() = std::move(res);
  }

  // Счётчики ID не меньше наибольших занятых ID.
  void counters() {
    std::array<u32, kSeqCount> maxId{};
    auto upd = [&](Seq s, Id id) { maxId[size_t(s)] = std::max(maxId[size_t(s)], id); };
    const World& x = w();
    x.nodes.each([&](const Node& e) { upd(Seq::Node, e.id); });
    x.edges.each([&](const Edge& e) { upd(Seq::Edge, e.id); });
    x.provinces.each([&](const Province& e) { upd(Seq::Province, e.id); });
    x.factions.each([&](const Faction& e) {
      upd(Seq::Faction, e.id);
      for (auto& r : e.army) upd(Seq::Row, r.id);
      for (auto& r : e.fleet) upd(Seq::Row, r.id);
      for (auto& s : e.council) upd(Seq::Council, s.id);
    });
    x.characters.each([&](const Character& e) { upd(Seq::Character, e.id); });
    x.modifiers.each([&](const Modifier& e) { upd(Seq::Modifier, e.id); });
    x.buildings.each([&](const Building& e) { upd(Seq::Building, e.id); });
    x.techs.each([&](const Tech& e) { upd(Seq::Tech, e.id); });
    x.armies.each([&](const Army& e) { upd(Seq::Army, e.id); });
    x.routes.each([&](const Route& e) { upd(Seq::Route, e.id); });
    x.deals.each([&](const Deal& e) { upd(Seq::Deal, e.id); });
    x.log.each([&](const LogEntry& e) { upd(Seq::Log, e.id); });
    const Catalogs& c = *x.catalogs;
    for (auto& i : c.resources) upd(Seq::Resource, i.id);
    for (auto& i : c.races) upd(Seq::Race, i.id);
    for (auto& i : c.cultures) upd(Seq::Culture, i.id);
    for (auto& i : c.religions) upd(Seq::Religion, i.id);
    for (auto& i : c.governments) upd(Seq::Government, i.id);
    for (auto& i : c.positions) upd(Seq::Position, i.id);
    static const char* const names[kSeqCount] = {"province", "faction", "character", "modifier", "building", "tech", "army",
                                                 "route", "deal", "log", "row", "council", "node", "edge", "resource", "race",
                                                 "culture", "religion", "government", "position"};
    for (int i = 0; i < kSeqCount; i++) {
      if (seq[size_t(i)] < maxId[size_t(i)]) {
        warn(F_WORLD, std::string("meta.seq.") + names[i], "счётчик " + std::to_string(seq[size_t(i)]) + " меньше наибольшего ID " +
                                                          std::to_string(maxId[size_t(i)]) + " — поднят");
        seq[size_t(i)] = maxId[size_t(i)];
      }
    }
    if (seq != x.meta->seq) tx.meta().seq = seq;
  }

  void run() {
    meta();
    catalogs();
    nodes();
    edges();
    factions();
    characters();
    modifiers();
    buildings();
    techs();
    provinces();
    armies();
    routes();
    deals();
    log();
    relations();
    counters();
  }
};

}  // namespace

u32 normalize(World& w, Warnings& warnings) {
  Tx tx(w);
  Norm n(tx, warnings);
  n.run();
  World out = std::move(tx).finish();
  u32 mask = World::diff(w, out);
  w = std::move(out);
  return mask;
}

}  // namespace rg::io
