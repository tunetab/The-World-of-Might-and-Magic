// Regnum — модель мира.
//
// World — НЕИЗМЕНЯЕМОЕ значение. Таблицы хранятся как разделяемые блоки (shared_ptr<const ...>):
// копия World стоит десятки байт, изменённые сущности копируются точечно. Поэтому:
//  - отмена/повтор — просто прежнее значение World;
//  - снимок хода — ещё одна ссылка на World (без сериализации);
//  - фоновые потоки (отрисовка тайлов) читают свой World без блокировок.
//
// Изменения выполняются только в транзакции хранилища:
//   store.transact("Переименовать провинцию", [&](Tx& tx) { tx.province(id).name = "Арден"; });
// Исключение внутри лямбды отменяет транзакцию целиком (ничего не применяется).
// UserError — понятная пользователю причина отказа (показывается как уведомление).
//
// Идентификаторы: целые > 0, отдельная последовательность для каждого вида (Meta::seq).
// Удалённые ID не переиспользуются. В файлах JSON записываются с префиксом (p12, f3, ...).
// Координаты карты — пиксели исходного изображения 8000 × 4500, ось Y вниз.
#pragma once
#include <map>
#include <unordered_map>

#include "base/base.h"

namespace rg {

using Id = u32;  // 0 — «нет»

// ================================================================ перечисления
enum class Terrain : u8 { None = 0, Land = 1, Sea = 2 };
enum class EdgeKind : u8 { Border = 0, Coast = 1, Frame = 2 };
enum class ProvSize : u8 { Small = 0, Medium = 1, Large = 2 };
enum class CityType : u8 { Outpost = 0, Village = 1, Town = 2, City = 3 };
enum class FactionKind : u8 { State = 0, Guild = 1 };
enum class UnitType : u8 { LightInf, MediumInf, HeavyInf, LightCav, MediumCav, HeavyCav, Flying, Casters, Ranged, Beasts, Monsters, Machines, Count };
enum class ShipType : u8 { ShipOfLine, Frigate, Galleon, Count };
enum class RelStatus : u8 { War = 0, Alliance = 1, Neutral = 2, Unknown = 3 };
enum class BuildingCat : u8 { Military = 0, Economic = 1, Industrial = 2, Residential = 3, Count };
enum class ArmyKind : u8 { Army = 0, Fleet = 1 };
enum class DealKind : u8 { Trade = 0, Tribute = 1, Reparations = 2 };
enum class DealSide : u8 { A = 0, B = 1 };
enum class DealMode : u8 { Once = 0, PerTurn = 1 };
enum class DealStatus : u8 { Active = 0, Done = 1, Cancelled = 2 };
enum class OccupiedIncome : u8 { Owner = 0, Occupier = 1, None = 2 };
enum class LogKind : u8 { Turn, Economy, Build, Tech, War, Battle, Army, Fleet, Diplomacy, Trade, Province, Guild, Population, Note, Count };

// Эффекты модификаторов (ТЗ 1.g.ii). Пределы и подписи — schema.h (kEffects).
enum class Fx : u8 {
  PopGrowthPct, TradePct, TradeFlat, BuildCostPct, ContentmentPerTurn, RebellionPct, ResourcePct, ResourceFlat, Slots,  // локальные
  IncomePct, DiplomacyPerTurn, ArmyUpkeepPct, FleetUpkeepPct,                                                       // глобальные
  Count
};
constexpr int kFxCount = int(Fx::Count);

// Последовательности идентификаторов.
enum class Seq : u8 {
  Province, Faction, Character, Modifier, Building, Tech, Army, Route, Deal, Log, Row, Council,
  Node, Edge, Resource, Race, Culture, Religion, Government, Position, Count
};
constexpr int kSeqCount = int(Seq::Count);

constexpr Id kGold = 1;  // встроенный ресурс «Золото» = казна

// ================================================================ сущности
struct Node { Id id = 0; Vec2 p; };

// Дуга плоского графа провинций: полилиния a -> pts... -> b.
// pl/pr — провинция слева/справа по ходу a->b (0 — не назначено); tl/tr — суша/море по сторонам.
// Береговые дуги (Coast) задаются базовой картой и не редактируются пользователем.
struct Edge {
  Id id = 0;
  Id a = 0, b = 0;
  std::vector<Vec2> pts;  // промежуточные точки (без a и b)
  EdgeKind kind = EdgeKind::Border;
  Id pl = 0, pr = 0;
  Terrain tl = Terrain::Land, tr = Terrain::Land;
};

struct GarrisonEntry { Id row = 0; i64 count = 0; };       // строка армии владельца
struct RacePop { Id race = 0; i64 pop = 0; };
struct Influence { Id guild = 0; double pct = 0; };          // торговое влияние гильдии, %
struct ProvBuilding {
  Id building = 0;
  int level = 1;            // уровень, который построен или строится
  bool constructing = false;
  int left = 0;             // ходов до завершения
  // Строящийся уровень: что фактически уплачено при начале строительства и кем (возврат при отмене — ровно это
  // и тому же плательщику). У достроенной постройки пусто; payer 0 — плательщик неизвестен или упразднён.
  std::map<Id, double> paid;
  Id payer = 0;
  int builtLevel() const { return constructing ? level - 1 : level; }  // действующий уровень (0 — ещё нет)
};

struct Province {
  Id id = 0;
  std::string name;
  Id owner = 0;             // государство
  Id lord = 0;              // персонаж
  bool sea = false;         // ТЗ 1.a.iii: морская провинция — без информации и без заливки
  std::string capital;      // название столицы
  ProvSize size = ProvSize::Medium;
  CityType city = CityType::Village;
  Id resource = 0;          // ресурс каталога
  double resourceAmount = 0;  // ≥ 0
  std::vector<GarrisonEntry> garrison;
  double contentment = 0;   // -100..100
  Id culture = 0, religion = 0;
  std::vector<RacePop> races;
  std::vector<Influence> influence;   // сумма ≤ 100
  std::vector<Id> hqs;      // гильдии со штабом (≤ 5)
  double baseTrade = 0;     // базовая торговая ценность
  double localTax = 0;      // местный налог, % (может быть < 0)
  std::vector<ProvBuilding> buildings;
  std::vector<Id> modifiers;
  bool occupied = false;
  Id occupier = 0;
  std::string notes;
  std::string entity;       // ID карточки кампании (необязательно)
};

enum class FlagPattern : u8 { Solid, H2, H3, V2, V3, Cross, Saltire, Quarters, Bend, Chevron, Border, Canton, Chief, Pale, Count };
struct Flag {
  bool image = false;            // true — изображение PNG, иначе конструктор
  FlagPattern pattern = FlagPattern::Solid;
  std::array<Color, 3> colors{Color::hex(0x7a4fd6), Color::hex(0xf2e3b3), Color::hex(0x1d2333)};
  std::string emblem;            // имя эмблемы (gfx/emblems) или пусто
  Color emblemColor = Color::hex(0xf2e3b3);
  std::string png;               // байты PNG, если image
};

struct ArmyRow { Id id = 0; std::string name; UnitType type = UnitType::LightInf; i64 total = 0; double upkeep = 0; };
struct FleetRow { Id id = 0; std::string name; ShipType type = ShipType::Frigate; i64 total = 0; double upkeep = 0; };
struct CouncilSeat { Id id = 0; std::string position; Id character = 0; };

struct Faction {
  Id id = 0;
  FactionKind kind = FactionKind::State;
  std::string name;
  Color color = Color::hex(0x7a4fd6);
  Flag flag;
  Id culture = 0, government = 0, religion = 0;
  Id ruler = 0;                  // персонаж
  std::string rulerTitle;
  std::vector<CouncilSeat> council;
  Id capital = 0;                // провинция (государство)
  std::vector<ArmyRow> army;
  std::vector<FleetRow> fleet;
  std::map<Id, double> res;      // запасы ресурсов; res[kGold] — казна
  std::vector<Id> modifiers;
  double tax = 10;               // налог государства, % (≥ 0)
  Id homeState = 0;              // гильдия: государство расположения
  bool stateGuild = false;       // гильдия: государственная
  std::string notes;
  std::string entity;

  bool isState() const { return kind == FactionKind::State; }
  bool isGuild() const { return kind == FactionKind::Guild; }
  double treasury() const { auto it = res.find(kGold); return it == res.end() ? 0 : it->second; }
  double stock(Id r) const { auto it = res.find(r); return it == res.end() ? 0 : it->second; }
  const ArmyRow* armyRow(Id row) const { for (auto& r : army) if (r.id == row) return &r; return nullptr; }
  const FleetRow* fleetRow(Id row) const { for (auto& r : fleet) if (r.id == row) return &r; return nullptr; }
};

struct Character {
  Id id = 0;
  std::string name, title;
  Id faction = 0;
  bool hero = false;             // значимый герой фракции
  double upkeep = 0;             // содержание за ход (расход «специалисты»)
  std::string portrait;          // байты PNG/JPEG или пусто
  std::string notes, entity;
};

struct Modifier {
  Id id = 0;
  std::string name;
  std::string icon = "sparkles";
  Color color = Color::hex(0x8c7ae6);
  std::string desc;
  std::array<double, kFxCount> fx{};  // значения эффектов
  u32 fxMask = 0;                     // какие эффекты заданы (бит = int(Fx))
  std::vector<Id> targets;            // цели эффекта «дипломатия»
  bool has(Fx f) const { return (fxMask >> int(f)) & 1u; }
  double get(Fx f) const { return has(f) ? fx[int(f)] : 0.0; }
};

struct BuildingLevel {
  int turns = 1;
  std::map<Id, double> cost;          // ресурс -> количество
  std::vector<Id> modifiers;
  std::string desc;
};
struct BuildingReq { Id building = 0; int level = 1; };
struct Building {
  Id id = 0;
  Id owner = 0;                       // 0 — общее дерево; иначе уникальная постройка фракции
  std::string name;
  std::string icon = "building";
  BuildingCat cat = BuildingCat::Economic;
  std::string desc;
  std::vector<BuildingReq> requires_;
  std::vector<BuildingLevel> levels{BuildingLevel{}};
  Vec2 pos;                           // положение на схеме дерева
};

struct Tech {
  Id id = 0;
  Id faction = 0;                     // дерево технологий фракции
  std::string name, desc;
  int turns = 1;
  std::vector<Id> prereqs;
  std::vector<Id> modifiers;
  bool studied = false;
  bool research = false;              // исследуется сейчас
  int progress = 0;                   // пройдено ходов
  Vec2 pos;                           // положение на схеме дерева
};

struct ArmyUnit { Id row = 0; i64 count = 0; };
struct ArmyGroup {                    // отряды одной фракции в объекте (союзное войско — несколько групп)
  Id faction = 0;
  std::vector<ArmyUnit> units;
  std::vector<Id> heroes;
};
struct Army {
  Id id = 0;
  ArmyKind kind = ArmyKind::Army;
  std::string name;
  Vec2 pos;
  std::vector<ArmyGroup> groups;
  Id commander = 0;                   // главный полководец/флотоводец
  bool isFleet() const { return kind == ArmyKind::Fleet; }
  bool allied() const { return groups.size() > 1; }
  Id leader() const { return groups.empty() ? 0 : groups[0].faction; }
};

struct Route {
  Id id = 0;
  std::string name;
  Id guild = 0;                       // гильдия-владелец (необязательно)
  std::vector<Vec2> pts;
  std::optional<Color> color;
};

struct DealItem {
  DealSide from = DealSide::A;
  Id res = kGold;
  double amount = 0;
  DealMode mode = DealMode::Once;
  int turns = 1;
  int left = 0;
};
struct Deal {
  Id id = 0;
  DealKind kind = DealKind::Trade;
  Id a = 0, b = 0;                    // стороны (для дани: a — получатель, b — плательщик)
  std::vector<DealItem> items;
  int turn = 1;                       // ход заключения
  DealStatus status = DealStatus::Active;
  std::string note;
};

struct LogEntry {
  Id id = 0;
  int turn = 1;
  LogKind kind = LogKind::Note;
  std::string text;
  Id province = 0, army = 0;
  std::vector<Id> factions;
  std::string at;                     // реальное время записи (ISO)
};

struct Relation {
  double v = 0;                       // -100..100
  RelStatus s = RelStatus::Unknown;
  bool operator==(const Relation&) const = default;
};
using RelMap = std::map<u64, Relation>;  // ключ relKey(a, b)
inline u64 relKey(Id a, Id b) { if (a > b) std::swap(a, b); return (u64(a) << 32) | b; }

struct CatalogItem {
  Id id = 0;
  std::string name;
  Color color = Color::hex(0x888888);
  std::string icon;
  bool builtin = false;
};
struct Catalogs {
  std::vector<CatalogItem> resources, races, cultures, religions, governments, positions;
  static const CatalogItem* find(const std::vector<CatalogItem>& list, Id id) {
    for (auto& c : list) if (c.id == id) return &c;
    return nullptr;
  }
};

struct Settings {
  float fillOpacity = 0.5f;
  bool labelStates = true, labelProvinces = true, labelArmies = true;
  OccupiedIncome occupiedIncome = OccupiedIncome::Owner;
  bool rebellionRoll = false;
  bool autosaveFolder = true;
  int autosaveSec = 60;
};

struct Meta {
  std::string name = "Новый мир";
  std::string createdAt, updatedAt;
  int turn = 1;
  std::array<u32, kSeqCount> seq{};
  std::string basemap = "wmm-expanded-v1";
  std::string notes;
};

// ================================================================ таблица
// Устойчивая таблица сущностей по ID: блоки по 64 записи, общие между версиями мира.
template <class T>
class Table {
 public:
  static constexpr u32 kBits = 6, kSize = 1u << kBits, kMask = kSize - 1;
  using Chunk = std::array<std::shared_ptr<const T>, kSize>;
  using Chunks = std::vector<std::shared_ptr<const Chunk>>;

  const T* get(Id id) const {
    if (!chunks_ || id == 0) return nullptr;
    u32 ci = id >> kBits;
    if (ci >= chunks_->size()) return nullptr;
    const auto& c = (*chunks_)[ci];
    return c ? (*c)[id & kMask].get() : nullptr;
  }
  bool has(Id id) const { return get(id) != nullptr; }
  u32 size() const { return count_; }
  bool empty() const { return count_ == 0; }
  // Обход по возрастанию ID: f(const T&).
  template <class F> void each(F&& f) const {
    if (!chunks_) return;
    for (const auto& c : *chunks_) {
      if (!c) continue;
      for (const auto& e : *c) if (e) f(*e);
    }
  }
  std::vector<Id> ids() const { std::vector<Id> r; r.reserve(count_); each([&](const T& e) { r.push_back(e.id); }); return r; }
  std::vector<const T*> all() const { std::vector<const T*> r; r.reserve(count_); each([&](const T& e) { r.push_back(&e); }); return r; }
  // Тождество версии: таблицы не менялись, если указатели блоков совпадают.
  bool same(const Table& o) const { return chunks_ == o.chunks_; }
  // Блоки, отличающиеся от другой версии (для точечной инвалидации кешей): вызывает f(id) для изменённых/удалённых/новых.
  template <class F> void diff(const Table& o, F&& f) const {
    size_t n = std::max(chunks_ ? chunks_->size() : 0, o.chunks_ ? o.chunks_->size() : 0);
    for (size_t ci = 0; ci < n; ci++) {
      auto a = chunks_ && ci < chunks_->size() ? (*chunks_)[ci] : nullptr;
      auto b = o.chunks_ && ci < o.chunks_->size() ? (*o.chunks_)[ci] : nullptr;
      if (a == b) continue;
      for (u32 k = 0; k < kSize; k++) {
        const T* x = a ? (*a)[k].get() : nullptr;
        const T* y = b ? (*b)[k].get() : nullptr;
        if (x != y) f(Id((ci << kBits) | k));
      }
    }
  }

 private:
  template <class> friend class TableEdit;
  std::shared_ptr<const Chunks> chunks_;
  u32 count_ = 0;
};

// Редактор таблицы внутри транзакции: копирует блоки и сущности при первом изменении.
template <class T>
class TableEdit {
 public:
  using Chunk = typename Table<T>::Chunk;
  using Chunks = typename Table<T>::Chunks;
  static constexpr u32 kBits = Table<T>::kBits, kMask = Table<T>::kMask;

  explicit TableEdit(Table<T>* t) : t_(t) {}

  bool touched() const { return chunks_ != nullptr; }

  T* mut(Id id) {
    const T* cur = t_->get(id);
    if (!cur) return nullptr;
    auto it = owned_.find(id);
    if (it != owned_.end()) return it->second;
    auto p = std::make_shared<T>(*cur);
    T* raw = p.get();
    chunkFor(id)[id & kMask] = std::move(p);
    owned_[id] = raw;
    return raw;
  }

  T& put(T value) {
    Id id = value.id;
    if (id == 0) throw std::logic_error("TableEdit::put: id = 0");
    bool existed = t_->get(id) != nullptr;
    auto p = std::make_shared<T>(std::move(value));
    T* raw = p.get();
    chunkFor(id)[id & kMask] = std::move(p);
    owned_[id] = raw;
    if (!existed) t_->count_++;
    return *raw;
  }

  bool erase(Id id) {
    if (!t_->get(id)) return false;
    chunkFor(id)[id & kMask].reset();
    owned_.erase(id);
    t_->count_--;
    return true;
  }

 private:
  Chunk& chunkFor(Id id) {
    if (!chunks_) {
      chunks_ = t_->chunks_ ? std::make_shared<Chunks>(*t_->chunks_) : std::make_shared<Chunks>();
      t_->chunks_ = chunks_;
    }
    u32 ci = id >> kBits;
    if (ci >= chunks_->size()) chunks_->resize(ci + 1);
    if (ci >= ownedChunks_.size()) ownedChunks_.resize(ci + 1);
    if (!ownedChunks_[ci]) {
      auto c = (*chunks_)[ci] ? std::make_shared<Chunk>(*(*chunks_)[ci]) : std::make_shared<Chunk>();
      ownedChunks_[ci] = c;
      (*chunks_)[ci] = c;
    }
    return *ownedChunks_[ci];
  }

  Table<T>* t_;
  std::shared_ptr<Chunks> chunks_;
  std::vector<std::shared_ptr<Chunk>> ownedChunks_;
  std::unordered_map<Id, T*> owned_;
};

// ================================================================ мир
enum TableBit : u32 {
  TB_META = 1u << 0, TB_SETTINGS = 1u << 1, TB_CATALOGS = 1u << 2, TB_NODES = 1u << 3, TB_EDGES = 1u << 4,
  TB_PROVINCES = 1u << 5, TB_FACTIONS = 1u << 6, TB_CHARACTERS = 1u << 7, TB_RELATIONS = 1u << 8,
  TB_MODIFIERS = 1u << 9, TB_BUILDINGS = 1u << 10, TB_TECHS = 1u << 11, TB_ARMIES = 1u << 12,
  TB_ROUTES = 1u << 13, TB_DEALS = 1u << 14, TB_LOG = 1u << 15,
  TB_GEO = TB_NODES | TB_EDGES, TB_ALL = 0xFFFFu
};

struct World {
  std::shared_ptr<const Meta> meta = std::make_shared<Meta>();
  std::shared_ptr<const Settings> settings = std::make_shared<Settings>();
  std::shared_ptr<const Catalogs> catalogs = std::make_shared<Catalogs>();
  Table<Node> nodes;
  Table<Edge> edges;
  Table<Province> provinces;
  Table<Faction> factions;
  Table<Character> characters;
  std::shared_ptr<const RelMap> relations = std::make_shared<RelMap>();
  Table<Modifier> modifiers;
  Table<Building> buildings;
  Table<Tech> techs;
  Table<Army> armies;
  Table<Route> routes;
  Table<Deal> deals;
  Table<LogEntry> log;

  int turn() const { return meta->turn; }
  const Province* province(Id id) const { return provinces.get(id); }
  const Faction* faction(Id id) const { return factions.get(id); }
  const Character* character(Id id) const { return characters.get(id); }
  const Modifier* modifier(Id id) const { return modifiers.get(id); }
  const Building* building(Id id) const { return buildings.get(id); }
  const Tech* tech(Id id) const { return techs.get(id); }
  const Army* army(Id id) const { return armies.get(id); }
  const Route* route(Id id) const { return routes.get(id); }
  const Deal* deal(Id id) const { return deals.get(id); }
  Relation relation(Id a, Id b) const;                 // по умолчанию: 0, «незнакомы»; a == b — союз 100
  const CatalogItem* resource(Id id) const { return Catalogs::find(catalogs->resources, id); }
  std::string factionName(Id id) const;                // «—» если нет
  std::string provinceName(Id id) const;
  std::string characterName(Id id) const;

  // Битовая маска таблиц, отличающихся между двумя версиями мира.
  static u32 diff(const World& a, const World& b);
};

// Новый мир с настройками и каталогами по умолчанию (без геометрии провинций).
World newWorld(const std::string& name);

// ================================================================ транзакция
class Tx {
 public:
  explicit Tx(const World& base);
  Tx(const Tx&) = delete;
  Tx& operator=(const Tx&) = delete;

  const World& w() const { return w_; }   // текущее состояние внутри транзакции
  u32 touched() const { return touched_; }

  Meta& meta();
  Settings& settings();
  Catalogs& catalogs();
  RelMap& relations();
  Id nextId(Seq s);                         // выдать следующий ID последовательности

  // Изменение сущности (копия при первом обращении). Нет такой сущности — UserError.
  Node& node(Id id);
  Edge& edge(Id id);
  Province& province(Id id);
  Faction& faction(Id id);
  Character& character(Id id);
  Modifier& modifier(Id id);
  Building& building(Id id);
  Tech& tech(Id id);
  Army& army(Id id);
  Route& route(Id id);
  Deal& deal(Id id);

  // Добавление: ID выдаётся автоматически (поле id заполняется), если value.id == 0.
  Node& add(Node v);
  Edge& add(Edge v);
  Province& add(Province v);
  Faction& add(Faction v);
  Character& add(Character v);
  Modifier& add(Modifier v);
  Building& add(Building v);
  Tech& add(Tech v);
  Army& add(Army v);
  Route& add(Route v);
  Deal& add(Deal v);
  LogEntry& add(LogEntry v);

  // Удаление записи без очистки ссылок (ссылки чистит rules::entities).
  void eraseNode(Id id);
  void eraseEdge(Id id);
  void eraseProvince(Id id);
  void eraseFaction(Id id);
  void eraseCharacter(Id id);
  void eraseModifier(Id id);
  void eraseBuilding(Id id);
  void eraseTech(Id id);
  void eraseArmy(Id id);
  void eraseRoute(Id id);
  void eraseDeal(Id id);

  // Отношения пары (симметричны).
  void setRelation(Id a, Id b, Relation r);

  // Заменить мир целиком (возврат к снимку хода) — отменяемо, как любая транзакция.
  // Правки, сделанные в этой транзакции до вызова, отбрасываются; после вызова можно продолжать менять мир.
  void replaceWorld(const World& w);

  // Итог транзакции (вызывает Store).
  World finish() &&;

 private:
  World w_;
  u32 touched_ = 0;
  bool metaOwned_ = false, settingsOwned_ = false, catalogsOwned_ = false, relOwned_ = false;
  std::shared_ptr<Meta> meta_;
  std::shared_ptr<Settings> settings_;
  std::shared_ptr<Catalogs> catalogs_;
  std::shared_ptr<RelMap> rel_;
  TableEdit<Node> nodes_;
  TableEdit<Edge> edges_;
  TableEdit<Province> provinces_;
  TableEdit<Faction> factions_;
  TableEdit<Character> characters_;
  TableEdit<Modifier> modifiers_;
  TableEdit<Building> buildings_;
  TableEdit<Tech> techs_;
  TableEdit<Army> armies_;
  TableEdit<Route> routes_;
  TableEdit<Deal> deals_;
  TableEdit<LogEntry> log_;
};

// ================================================================ хранилище
struct TxOptions {
  std::string coalesce;          // непустой ключ: подряд идущие транзакции объединяются в одну запись отмены
  double coalesceSec = 1.5;
  bool history = true;           // false — не попадает в стек отмены
};

struct Change {
  enum Kind { Commit, Undo, Redo, Load } kind = Commit;
  std::string label;
  u32 tables = 0;                // TableBit
  const World* before = nullptr; // предыдущее состояние (действительно только во время уведомления)
  const World* after = nullptr;
};

class Store {
 public:
  Store();
  const World& world() const { return cur_; }
  u64 version() const { return version_; }

  // Выполнить изменение мира. fn(Tx&) может вернуть значение. Исключение — откат.
  template <class F>
  decltype(auto) transact(std::string_view label, F&& fn, const TxOptions& opt = {}) {
    Tx tx(cur_);
    if constexpr (std::is_void_v<decltype(fn(tx))>) {
      fn(tx);
      commit(label, std::move(tx), opt);
    } else {
      auto result = fn(tx);
      commit(label, std::move(tx), opt);
      return result;
    }
  }

  bool canUndo() const { return !undo_.empty(); }
  bool canRedo() const { return !redo_.empty(); }
  std::string undoLabel() const { return undo_.empty() ? std::string() : undo_.back().label; }
  std::string redoLabel() const { return redo_.empty() ? std::string() : redo_.back().label; }
  bool undo();
  bool redo();
  void endCoalesce();
  void clearHistory();

  // Заменить мир целиком (открытие файла, восстановление снимка). Очищает отмену.
  void replace(World w, std::string_view label, bool markSaved = true);

  void markSaved();
  u32 dirtyTables() const { return World::diff(cur_, saved_); }
  bool dirty() const { return dirtyTables() != 0; }
  const World& savedWorld() const { return saved_; }

  // Подписка на изменения. Возвращает номер для отписки.
  int subscribe(std::function<void(const Change&)> fn);
  void unsubscribe(int token);

  size_t limit = 400;

 private:
  void commit(std::string_view label, Tx&& tx, const TxOptions& opt);
  void notify(const Change& c);

  struct Entry { std::string label; World before, after; std::string coalesce; double time = 0; };
  World cur_, saved_;
  std::vector<Entry> undo_, redo_;
  u64 version_ = 1;
  std::vector<std::pair<int, std::function<void(const Change&)>>> subs_;
  int nextSub_ = 1;
};

}  // namespace rg
