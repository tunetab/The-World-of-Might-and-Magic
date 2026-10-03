// Regnum — общие виджеты предметной области для панелей, диалогов и редакторов.
// Единый вид выбора фракций, провинций, персонажей, записей справочников и модификаторов,
// «фишки» сущностей со щелчком-переходом и подписи эффектов. Только из главного потока внутри кадра.
//
//   Id owner = p->owner;
//   if (w::factionPicker("owner", owner, w::FactionFilter::States, "Без владельца"))
//     a.act("Владелец провинции", [&](Tx& tx) { rules::setProvinceOwner(tx, pid, owner); });
#pragma once
#include "app/app.h"

namespace rg::app::w {

enum class FactionFilter : u8 { Any, States, Guilds };

// Выпадающий выбор фракции (цветная точка, значок вида). value — ID или 0 («нет», если noneLabel не пуст).
// exclude — скрыть фракцию (например, саму себя). true — значение изменилось.
bool factionPicker(std::string_view id, Id& value, FactionFilter filter = FactionFilter::Any,
                   std::string_view noneLabel = "—", Id exclude = 0, bool disabled = false);

// Выбор провинции (owner != 0 — только провинции этого государства). Морские провинции не предлагаются.
bool provincePicker(std::string_view id, Id& value, Id owner = 0, std::string_view noneLabel = "—", bool disabled = false);

// Выбор персонажа. faction != 0 — сначала персонажи этой фракции (остальные ниже).
// allowCreate — последний пункт «Новый персонаж»: создаёт запись (с фракцией faction) и выбирает её.
bool characterPicker(std::string_view id, Id& value, Id faction = 0, std::string_view noneLabel = "—",
                     bool allowCreate = true, bool disabled = false);

// Выбор записи справочника (ресурсы, расы, культуры, религии, формы правления, должности).
// allowCreate — пункт «Добавить…»: запрос названия и создание записи.
bool catalogPicker(std::string_view id, rules::CatalogList list, Id& value, std::string_view noneLabel = "—",
                   bool allowCreate = false, bool disabled = false);

// Где действует список модификаторов (ТЗ 1.g.ii): Local — провинция (действуют только локальные эффекты: модификатор
// лишь с глобальными эффектами помечается предупреждением и не предлагается к добавлению); Any — технологии и
// постройки (локальные эффекты — в провинции, глобальные — государству).
enum class ModScope { Any, Local };
// Список модификаторов фишками (удаление крестиком) + выбор для добавления. true — список изменился.
// Фишка без действующих здесь эффектов помечается в App::uiRect как «<id>.warn.<модификатор>».
bool modifierList(std::string_view id, std::vector<Id>& ids, bool disabled = false, ModScope where = ModScope::Any);
// У модификатора есть эффекты, но ни один не действует там, где список (where).
bool modifierInert(const Modifier& m, ModScope where);

// Фишки-ссылки: щелчок выделяет сущность и открывает её инспектор.
void factionChip(Id faction, bool showKind = false);
void provinceChip(Id province);
void characterChip(Id character);

// Подпись эффекта: «+10 % торговой ценности», «−5 довольства за ход».
std::string effectText(Fx f, double value);
// Фишки всех эффектов модификатора (значок эффекта, знак, тон: польза — success, вред — danger).
void effectChips(const Modifier& m);
// Хорош ли эффект для владельца при данном знаке (рост восстания и стоимости — плохо).
bool effectGood(Fx f, double value);

// Значок и цвет ресурса (из справочника; «Золото» — coins).
const char* resourceIcon(const World& w, Id res);
Color resourceColor(const World& w, Id res);
// Строка «значок + количество» для ресурса (например, стоимость постройки).
void resourceAmount(Id res, double amount, ui::Ink ink = ui::Ink::Normal);

// Цвет фракции (серый, если нет).
Color factionColor(const World& w, Id faction);

}  // namespace rg::app::w
