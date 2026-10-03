// Regnum — таблицы перечислений и эффектов.
#include "core/schema.h"

namespace rg::schema {

const EnumInfo kUnitTypes[int(UnitType::Count)] = {
  {"light_inf", "Лёгкая пехота", "u-light-inf"},
  {"medium_inf", "Средняя пехота", "u-medium-inf"},
  {"heavy_inf", "Тяжёлая пехота", "u-heavy-inf"},
  {"light_cav", "Лёгкая кавалерия", "u-light-cav"},
  {"medium_cav", "Средняя кавалерия", "u-medium-cav"},
  {"heavy_cav", "Тяжёлая кавалерия", "u-heavy-cav"},
  {"flying", "Летающие отряды", "u-flying"},
  {"casters", "Колдующие отряды", "u-casters"},
  {"ranged", "Стрелки", "u-ranged"},
  {"beasts", "Звери", "u-beasts"},
  {"monsters", "Чудовища", "u-monsters"},
  {"machines", "Военные механизмы", "u-machines"},
};

const EnumInfo kShipTypes[int(ShipType::Count)] = {
  {"ship_line", "Линкор", "s-ship-line", 1},
  {"frigate", "Фрегат", "s-frigate", 1},
  {"galleon", "Торговый галеон", "s-galleon", 0},
};

const EnumInfo kRelStatus[4] = {
  {"war", "В войне", "war", 0, 0xd0573f},
  {"alliance", "В союзе", "alliance", 0, 0x4f9d69},
  {"neutral", "Статус-кво", "status-quo", 0, 0x9aa3b2},
  {"unknown", "Незнакомы", "unknown", 0, 0x6b7280},
};

const EnumInfo kProvSizes[3] = {
  {"small", "Маленькая", "size-s", 1},
  {"medium", "Средняя", "size-m", 2},
  {"large", "Большая", "size-l", 3},
};

const EnumInfo kCityTypes[4] = {
  {"outpost", "Аванпост", "c-outpost", 0},
  {"village", "Деревня", "c-village", 1},
  {"town", "Небольшой город", "c-town", 2},
  {"city", "Большой город", "c-city", 3},
};

const EnumInfo kBuildingCats[int(BuildingCat::Count)] = {
  {"military", "Военные", "b-military", 0, 0xd0573f},
  {"economic", "Экономические", "b-economic", 0, 0xd6a531},
  {"industrial", "Промышленные", "b-industrial", 0, 0x7f8a99},
  {"residential", "Жилые", "b-residential", 0, 0x4f9d69},
};

const EnumInfo kFactionKinds[2] = {
  {"state", "Государство", "crown"},
  {"guild", "Торговая гильдия", "guild"},
};

const EnumInfo kDealKinds[3] = {
  {"trade", "Торговая сделка", "trade"},
  {"tribute", "Дань", "tribute"},
  {"reparations", "Репарации", "reparations"},
};

const EnumInfo kDealModes[2] = {
  {"once", "Разово", "bolt"},
  {"turn", "Каждый ход", "repeat"},
};

const EnumInfo kLogKinds[int(LogKind::Count)] = {
  {"turn", "Ход", "hourglass"},
  {"economy", "Экономика", "treasury"},
  {"build", "Строительство", "build"},
  {"tech", "Технологии", "tech"},
  {"war", "Война", "war"},
  {"battle", "Битва", "battle"},
  {"army", "Войска", "army"},
  {"fleet", "Флот", "fleet"},
  {"diplomacy", "Дипломатия", "diplomacy"},
  {"trade", "Торговля", "trade"},
  {"province", "Провинция", "province"},
  {"guild", "Гильдия", "guild"},
  {"population", "Население", "population"},
  {"note", "Запись", "note"},
};

const EnumInfo kFlagPatterns[int(FlagPattern::Count)] = {
  {"solid", "Однотонный", "flag"}, {"h2", "Две полосы", "flag"}, {"h3", "Три полосы", "flag"},
  {"v2", "Два столбца", "flag"}, {"v3", "Три столбца", "flag"}, {"cross", "Крест", "flag"},
  {"saltire", "Косой крест", "flag"}, {"quarters", "Четверти", "flag"}, {"bend", "Перевязь", "flag"},
  {"chevron", "Стропило", "flag"}, {"border", "Кайма", "flag"}, {"canton", "Крыж", "flag"},
  {"chief", "Глава", "flag"}, {"pale", "Столб", "flag"},
};

const EnumInfo kOccupiedIncome[3] = {
  {"owner", "Владельцу", "crown"},
  {"occupier", "Оккупанту", "occupied"},
  {"none", "Никому", "close"},
};

const EffectInfo kEffects[kFxCount] = {
  {Fx::PopGrowthPct, "popGrowthPct", "Прирост населения за ход", "%", -50, 50, true, true, false, false, "population"},
  {Fx::TradePct, "tradePct", "Торговая ценность, % от базовой", "%", -100, 100, true, false, false, false, "trade-value"},
  {Fx::TradeFlat, "tradeFlat", "Торговая ценность, количество", "", -5000, 5000, true, false, false, false, "trade-value"},
  {Fx::BuildCostPct, "buildCostPct", "Стоимость строительства", "%", -100, 100, true, false, false, false, "build-cost"},
  {Fx::ContentmentPerTurn, "contentmentPerTurn", "Довольство населения за ход", "", -25, 25, true, true, false, false, "contentment"},
  {Fx::RebellionPct, "rebellionPct", "Вероятность восстания", "%", -50, 50, true, false, false, false, "rebellion"},
  {Fx::ResourcePct, "resourcePct", "Добыча ресурса, %", "%", -100, 100, true, false, false, false, "resource"},
  {Fx::ResourceFlat, "resourceFlat", "Добыча ресурса, количество", "", -100, 100, true, false, false, false, "resource"},
  {Fx::Slots, "slots", "Слоты построек", "", -5, 5, true, false, false, true, "slots"},
  {Fx::IncomePct, "incomePct", "Доход в казну", "%", -50, 50, false, false, false, false, "treasury"},
  {Fx::DiplomacyPerTurn, "diplomacyPerTurn", "Отношения за ход", "", -25, 25, false, true, true, false, "diplomacy"},
  {Fx::ArmyUpkeepPct, "armyUpkeepPct", "Содержание войск", "%", -75, 75, false, false, false, false, "army-upkeep"},
  {Fx::FleetUpkeepPct, "fleetUpkeepPct", "Содержание флота", "%", -75, 75, false, false, false, false, "fleet-upkeep"},
};

const EnumInfo kMapModes[int(MapMode::Count)] = {
  {"political", "Политическая карта", "mode-political"},
  {"guilds", "Гильдии и торговля", "mode-guilds"},
  {"contentment", "Довольство", "contentment"},
  {"rebellion", "Риск восстания", "rebellion"},
  {"trade", "Торговая ценность", "trade-value"},
  {"resources", "Ресурсы", "resource"},
  {"religion", "Религии", "religion"},
  {"culture", "Культуры", "culture"},
  {"terrain", "Чистая карта", "mode-terrain"},
};

int findEnum(const EnumInfo* list, int n, std::string_view id) {
  for (int i = 0; i < n; i++) if (id == list[i].id) return i;
  return -1;
}

const char* idPrefix(Seq s) {
  switch (s) {
    case Seq::Province: return "p";
    case Seq::Faction: return "f";
    case Seq::Character: return "c";
    case Seq::Modifier: return "m";
    case Seq::Building: return "b";
    case Seq::Tech: return "t";
    case Seq::Army: return "a";
    case Seq::Route: return "r";
    case Seq::Deal: return "d";
    case Seq::Log: return "l";
    case Seq::Row: return "u";
    case Seq::Council: return "k";
    case Seq::Node: return "";
    case Seq::Edge: return "";
    case Seq::Resource: return "rs";
    case Seq::Race: return "rc";
    case Seq::Culture: return "cu";
    case Seq::Religion: return "rl";
    case Seq::Government: return "gv";
    case Seq::Position: return "po";
    default: return "x";
  }
}

}  // namespace rg::schema
