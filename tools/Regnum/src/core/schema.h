// Regnum — справочник перечислений: русские подписи, значки, пределы правил (ТЗ).
#pragma once
#include "core/world.h"

namespace rg::schema {

struct EnumInfo {
  const char* id;     // устойчивый идентификатор для JSON
  const char* name;   // подпись в интерфейсе
  const char* icon;   // имя значка (gfx/icons)
  int value = 0;      // слоты (величина, тип города) или иное число
  u32 color = 0;      // 0xRRGGBB, если нужен цвет
};

extern const EnumInfo kUnitTypes[int(UnitType::Count)];
extern const EnumInfo kShipTypes[int(ShipType::Count)];
extern const EnumInfo kRelStatus[4];
extern const EnumInfo kProvSizes[3];
extern const EnumInfo kCityTypes[4];
extern const EnumInfo kBuildingCats[int(BuildingCat::Count)];
extern const EnumInfo kFactionKinds[2];
extern const EnumInfo kDealKinds[3];
extern const EnumInfo kDealModes[2];
extern const EnumInfo kLogKinds[int(LogKind::Count)];
extern const EnumInfo kFlagPatterns[int(FlagPattern::Count)];
extern const EnumInfo kOccupiedIncome[3];

inline const EnumInfo& unitType(UnitType t) { return kUnitTypes[int(t)]; }
inline const EnumInfo& shipType(ShipType t) { return kShipTypes[int(t)]; }
inline const EnumInfo& relStatus(RelStatus s) { return kRelStatus[int(s)]; }
inline const EnumInfo& provSize(ProvSize s) { return kProvSizes[int(s)]; }
inline const EnumInfo& cityType(CityType c) { return kCityTypes[int(c)]; }
inline const EnumInfo& buildingCat(BuildingCat c) { return kBuildingCats[int(c)]; }
inline const EnumInfo& logKind(LogKind k) { return kLogKinds[int(k)]; }

// Найти значение перечисления по строковому id (для чтения JSON). Возвращает -1, если нет.
int findEnum(const EnumInfo* list, int n, std::string_view id);

// Эффекты модификаторов (ТЗ 1.g.ii).
struct EffectInfo {
  Fx fx;
  const char* id;
  const char* name;
  const char* unit;     // "%" или ""
  double min, max;
  bool local;           // true — провинция, false — государство/гильдия
  bool perTurn;         // применяется каждый ход
  bool targets;         // требует список целевых фракций (дипломатия)
  bool extra;           // не перечислен в ТЗ явно (слоты — из пункта 1.f.i)
  const char* icon;
};
extern const EffectInfo kEffects[kFxCount];
inline const EffectInfo& effect(Fx f) { return kEffects[int(f)]; }

// Режимы карты.
enum class MapMode : u8 { Political, Guilds, Contentment, Rebellion, Trade, Resources, Religion, Culture, Terrain, Count };
extern const EnumInfo kMapModes[int(MapMode::Count)];

// Префиксы ID в JSON: p12, f3, ... (узлы и рёбра геометрии — без префикса).
const char* idPrefix(Seq s);

// Константы правил (ТЗ).
constexpr int kMaxHqPerProvince = 5;          // 1.d.ii
constexpr double kWarRelation = -25;          // 1.c.iv: «равны 0 и ещё минус 25»
constexpr double kRouteBonus = 0.10;          // 1.d.v: +10 % базовой ценности за маршрут
constexpr double kMinTotalTax = 1.0;          // 1.d.iv: общий налог не меньше 1 %
constexpr double kMaxTotalTax = 100.0;        // общий налог не больше 100 % (налог не превышает торговую ценность)
constexpr double kRebellionPerContentment = 0.5;  // 1.a.vi: 2 довольства : 1 %
constexpr double kObjectRadius = 34;          // радиус фигурки войска/флота на карте (единицы карты)
constexpr double kMapWidth = 8000, kMapHeight = 4500;

}  // namespace rg::schema
