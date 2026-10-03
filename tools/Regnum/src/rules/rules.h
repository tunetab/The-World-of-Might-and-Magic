// Regnum — правила мира: расчёты (от const World&) и действия (через Tx&).
// Формулы и толкование ТЗ — docs/RULES.md. Нарушение правила — rg::fail("причина по-русски").
// Действия, являющиеся событиями мира, записывают хронику (addLog).
#pragma once
#include <map>
#include <unordered_map>

#include "core/schema.h"
#include "core/world.h"

namespace rg::rules {

// ================================================================ модификаторы
struct EffectSource {
  enum Kind : u8 { Province, Faction, Tech, Building, Guild } kind = Province;
  Id id = 0;         // провинция / фракция / технология / постройка / гильдия
  Id modifier = 0;
};
struct Effects {
  std::array<double, kFxCount> v{};
  std::map<Id, double> diplomacy;     // цель -> изменение отношений за ход
  std::vector<EffectSource> sources;  // откуда взялись (для подсказок)
  double operator[](Fx f) const { return v[int(f)]; }
};
Effects provinceEffects(const World& w, Id province);   // локальные эффекты
Effects factionEffects(const World& w, Id faction);     // глобальные эффекты

// ================================================================ расчёты
struct GuildShare { Id guild = 0; double pct = 0; bool hq = false; double gross = 0, tax = 0, net = 0; };

struct ProvinceCalc {
  Id id = 0;
  bool sea = false;
  Id owner = 0;
  Id recipient = 0;                   // кто получает доход (оккупация по настройкам)
  int slotsSize = 0, slotsCity = 0, slotsMods = 0, slots = 0, slotsUsed = 0;
  double tradeBase = 0, tradeValue = 0;
  int routes = 0;
  double production = 0;              // добыча ресурса за ход
  double rebellion = 0;               // 0..100
  double buildCostFactor = 1;
  double taxState = 0, taxLocal = 0, taxTotal = 0;   // %
  i64 population = 0;
  std::vector<std::pair<Id, i64>> races;             // раса -> численность (по убыванию)
  std::vector<GuildShare> guilds;
  double provinceTax = 0, guildTax = 0;              // в казну получателя
  Effects fx;
};

struct RowCalc {
  Id row = 0;
  i64 total = 0, field = 0, garrison = 0, reserve = 0;   // field включает гарнизоны
  double upkeepEach = 0, upkeepTotal = 0;
};

struct ResourceFlow { double stock = 0, production = 0, tradeIn = 0, tradeOut = 0, net = 0; };

struct FactionCalc {
  Id id = 0;
  std::vector<Id> provinces;          // государство: владения; гильдия: провинции штабов
  i64 population = 0;
  std::vector<std::pair<Id, i64>> races;               // по провинциям (ТЗ 1.b.iv — автоматически)
  double incProvinces = 0, incGuildTax = 0, incGuilds = 0, incTrade = 0, incTribute = 0;
  double incGross = 0, incomePct = 0, incTotal = 0;
  double expArmy = 0, expFleet = 0, expSpecialists = 0, expTrade = 0, expTribute = 0, expTotal = 0;
  double net = 0, treasury = 0;
  std::vector<RowCalc> army, fleet;
  i64 armyTotal = 0, armyField = 0, fleetTotal = 0, fleetField = 0;
  std::map<Id, ResourceFlow> resources;
  Effects fx;
};

struct Calc {
  std::unordered_map<Id, ProvinceCalc> provinces;
  std::unordered_map<Id, FactionCalc> factions;
  std::unordered_map<Id, int> routeCounts;          // провинция -> число маршрутов
  const ProvinceCalc* province(Id id) const { auto it = provinces.find(id); return it == provinces.end() ? nullptr : &it->second; }
  const FactionCalc* faction(Id id) const { auto it = factions.find(id); return it == factions.end() ? nullptr : &it->second; }
};
// Полный расчёт мира. Кеш по тождеству таблиц мира; потокобезопасно.
// Не вызывайте для tx.w() изменённой транзакции: таблицы черновика меняются на месте — используйте calc(tx).
std::shared_ptr<const Calc> calc(const World& w);
// Расчёт внутри транзакции: кеш используется, только пока транзакция ничего не меняла.
std::shared_ptr<const Calc> calc(const Tx& tx);

// Развёрнутые силы фракции: строка -> число в войсках, флотах и гарнизонах.
struct Deployed { std::map<Id, i64> army, fleet, garrison; };
Deployed deployed(const World& w, Id faction);

// ================================================================ сущности
Id createFaction(Tx& tx, FactionKind kind, const std::string& name = {});
void removeFaction(Tx& tx, Id faction);      // чистит владения, отношения, войска, сделки, штабы, ссылки
Id createCharacter(Tx& tx, Id faction = 0, const std::string& name = {});
void removeCharacter(Tx& tx, Id character);  // чистит лордов, правителей, совет, героев войск
Id createModifier(Tx& tx, const std::string& name = {});
void removeModifier(Tx& tx, Id modifier);    // убирает из всех списков
Id createBuilding(Tx& tx, Id owner /*0 — общее дерево*/, const std::string& name = {});
void removeBuilding(Tx& tx, Id building);    // убирает из провинций и требований
// Удалить последний уровень постройки (не построен и не строится нигде, остаётся хотя бы один). Требования других
// построек к удалённому уровню опускаются до нового наибольшего; возвращает ID таких построек.
std::vector<Id> removeLastBuildingLevel(Tx& tx, Id building);
Id createTech(Tx& tx, Id faction, const std::string& name = {});
void removeTech(Tx& tx, Id tech);            // убирает из зависимостей
void copyTechTree(Tx& tx, Id from, Id to);   // копия дерева технологий (без изученности)
enum class CatalogList : u8 { Resources, Races, Cultures, Religions, Governments, Positions };
Id addCatalogItem(Tx& tx, CatalogList list, const std::string& name);
void removeCatalogItem(Tx& tx, CatalogList list, Id item);   // чистит все ссылки; «Золото» удалить нельзя
std::vector<CatalogItem>& catalogList(Catalogs& c, CatalogList list);
const std::vector<CatalogItem>& catalogList(const Catalogs& c, CatalogList list);

void setProvinceOwner(Tx& tx, Id province, Id faction);     // гарнизон → резерв прежнего владельца, столица, оккупация
void setOccupied(Tx& tx, Id province, Id occupier /*0 — снять*/);
void deleteProvince(Tx& tx, Id province);                   // геометрия → «не назначено», запись и ссылки удаляются
void mergeProvinces(Tx& tx, Id target, Id source);          // геометрия и население/постройки source → target
Id splitProvince(Tx& tx, Id province, const std::vector<Vec2>& line);   // нож; новая провинция наследует владельца, культуру, религию
void setCapital(Tx& tx, Id state, Id province /*0 — снять*/);          // столичная провинция — только своя
// Морская/сухопутная провинция (ТЗ 1.a.iii). Ставшая морской: гарнизон → резерв, столица и оккупация снимаются,
// начатое строительство приостанавливается; прочие данные сохраняются и снова действуют на суше.
void setProvinceSea(Tx& tx, Id province, bool sea);

// Правка областей инструментами карты (geo::createProvince/addArea/removeArea/fillAt): провинции, у которых после
// правки не осталось области, удаляются вместе с записью (хроника, возврат за начатое строительство).
// province — новая/изменённая провинция (0, если она сама осталась без области); removed — названия удалённых.
struct AreaEdit { Id province = 0; std::vector<std::string> removed; };
AreaEdit createProvince(Tx& tx, const std::vector<Vec2>& poly, Terrain terrain = Terrain::None, double snap = 1.0);
AreaEdit addArea(Tx& tx, Id province, const std::vector<Vec2>& poly, double snap = 1.0);
AreaEdit removeArea(Tx& tx, Id province, const std::vector<Vec2>& poly, double snap = 1.0);
AreaEdit fillAt(Tx& tx, Vec2 p, Id province /*0 — новая провинция*/);

// Строки таблиц войск и флота фракции (ТЗ 1.c.i). ID строк — общая последовательность Seq::Row.
Id addArmyRow(Tx& tx, Id faction, UnitType type, const std::string& name = {}, i64 total = 0, double upkeep = 0);
Id addFleetRow(Tx& tx, Id faction, ShipType type, const std::string& name = {}, i64 total = 0, double upkeep = 0);
void setRowTotal(Tx& tx, Id faction, Id row, i64 total);   // не меньше числа в поле (войска и гарнизоны)
void removeRow(Tx& tx, Id faction, Id row);                 // отряды строки убираются из войск и гарнизонов

// ================================================================ войска и флот
Id createArmy(Tx& tx, ArmyKind kind, Id faction, Vec2 pos);
void renameArmy(Tx& tx, Id army, const std::string& name);
void setUnits(Tx& tx, Id army, Id faction, Id row, i64 count);       // не больше резерва
void setHero(Tx& tx, Id army, Id character, bool on);
void setCommander(Tx& tx, Id army, Id character);
void setGarrison(Tx& tx, Id province, Id row, i64 count);            // строка армии владельца
void disband(Tx& tx, Id army);                                        // отряды → резерв
void moveArmy(Tx& tx, Id army, Vec2 pos);                             // проверка рельефа и наложения

enum class EncounterType : u8 { None, Merge, Battle, Alliance, DeclareWar, Blocked };
struct Encounter {
  EncounterType type = EncounterType::None;
  Id target = 0;
  std::string reason;
  Id us = 0, them = 0;   // пара фракций: Battle — воюющие, DeclareWar — кому предложить объявить войну
};
// Что произойдёт, если объект moving поставить в pos (с учётом объекта под позицией).
// Встреча — наложение фигурок (ближе 2 · kObjectRadius); None — свободное перемещение.
Encounter encounter(const World& w, Id moving, Vec2 pos);
void mergeArmies(Tx& tx, Id target, Id source);       // одна фракция: численности складываются
void formAllied(Tx& tx, Id target, Id source);        // союзники: группы хранятся раздельно
std::vector<Id> dissolveAllied(Tx& tx, Id army);      // группы → отдельные объекты рядом; ID всех объектов, исходный первым
struct SplitSpec { std::map<std::pair<Id, Id>, i64> units; std::vector<Id> heroes; };   // (фракция, строка) -> число
Id splitArmy(Tx& tx, Id army, const SplitSpec& spec);  // новый объект рядом
void declareWar(Tx& tx, Id a, Id b);                  // «в войне», отношения −25 (ТЗ 1.c.iv)

struct BattleResult {
  Id attacker = 0, defender = 0;
  bool attackerWins = true;
  Vec2 attackerOrigin;                                  // откуда пришёл нападавший
  std::map<Id, std::map<std::pair<Id, Id>, i64>> losses;  // армия -> (фракция, строка) -> потери
};
void resolveBattle(Tx& tx, const BattleResult& r);      // потери, смещение проигравшего, хроника
// Проверки размещения берут грани из кеша geo::faces: передавайте значение мира, а не черновик с изменённой геометрией.
std::optional<Vec2> findFreeSpot(const World& w, ArmyKind kind, Vec2 near, Id exclude = 0);   // спираль от near
bool validPosition(const World& w, ArmyKind kind, Vec2 pos, Id exclude = 0, std::string* why = nullptr);
Id armyAt(const World& w, Vec2 pos, Id exclude = 0);   // объект, фигурка которого покрывает точку

// ================================================================ дипломатия
void setRelation(Tx& tx, Id a, Id b, double value, RelStatus status);
struct RelationRow { Id other = 0; double value = 0; RelStatus status = RelStatus::Unknown; };
std::vector<RelationRow> relationsOf(const World& w, Id faction);   // все прочие фракции

// ================================================================ торговля, дань, репарации
struct DealCheck { bool ok = true; std::vector<std::string> problems; };
DealCheck validateDeal(const World& w, const Deal& d);
Id concludeDeal(Tx& tx, Deal d);                       // разовые позиции исполняются сразу
void cancelDeal(Tx& tx, Id deal);
Id imposeTribute(Tx& tx, DealKind kind, Id receiver, Id payer, double amountPerTurn, int turns);

// ================================================================ строительство
struct BuildOption {
  Id building = 0;
  int level = 1;                     // уровень, который будет строиться
  bool upgrade = false;
  int turns = 1;
  std::map<Id, double> cost;         // с учётом множителя провинции
  bool can = false;
  std::vector<std::string> reasons;  // почему нельзя
};
std::vector<BuildOption> buildOptions(const World& w, Id province);
void startBuilding(Tx& tx, Id province, Id building);  // новый или следующий уровень
void cancelBuilding(Tx& tx, Id province, Id building); // полный возврат стоимости
void demolish(Tx& tx, Id province, Id building);

// ================================================================ технологии
struct ResearchCheck { bool ok = false; std::vector<Id> missing; };
ResearchCheck canResearch(const World& w, Id tech);
void setStudied(Tx& tx, Id tech, bool studied);
void startResearch(Tx& tx, Id tech);
void stopResearch(Tx& tx, Id tech);
bool wouldCycle(const World& w, Id tech, Id prereq);
void setPrereq(Tx& tx, Id tech, Id prereq, bool on);  // с проверкой цикла
void autoLayout(Tx& tx, Id faction);                  // расстановка дерева по слоям
constexpr double kTreeColStep = 280, kTreeRowStep = 120;  // шаг столбцов (слоёв) и строк autoLayout

// ================================================================ гильдии и маршруты
void buildHq(Tx& tx, Id guild, Id province);
void removeHq(Tx& tx, Id guild, Id province);
void setInfluence(Tx& tx, Id province, Id guild, double pct);   // сумма по провинции ≤ 100
void setHomeState(Tx& tx, Id guild, Id state);                  // запрещено для государственной гильдии
Id createStateGuild(Tx& tx, Id state, const std::string& name = {});
Id createRoute(Tx& tx, const std::vector<Vec2>& pts, Id guild = 0);
void setRoutePoints(Tx& tx, Id route, const std::vector<Vec2>& pts);
void removeRoute(Tx& tx, Id route);

// ================================================================ хроника и ход
struct LogRefs { Id province = 0, army = 0; std::vector<Id> factions; };
Id addLog(Tx& tx, LogKind kind, const std::string& text, const LogRefs& refs = {});

struct TurnFactionLine { Id faction = 0; double treasuryBefore = 0, treasuryAfter = 0, income = 0, expenses = 0; std::map<Id, double> resources; };
struct TurnReport {
  int turnFrom = 0, turnTo = 0;
  std::vector<TurnFactionLine> factions;
  std::vector<Id> logIds;            // записи хроники, созданные при завершении хода
  std::vector<Id> rebellions;        // провинции, где вспыхнуло восстание
};
// Шаг 1 (снимок мира в истории ходов) выполняет вызывающий: снимок — store.world() до транзакции (core/io).
TurnReport endTurn(Tx& tx);
TurnReport previewTurn(const World& w);   // без изменения мира; logIds указывают на записи пробного хода

}  // namespace rg::rules
