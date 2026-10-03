// Regnum — общие помощники панелей государств и гильдий (инспектор, списки, флаг).
// Только для src/app/panels/faction*.cpp и src/app/dialogs/flag_editor.cpp.
#pragma once
#include "app/app_internal.h"
#include "app/widgets.h"

namespace rg::app::fac {

// Снимок мира на время кадра: указатели на записи (и флаги в ui::flag) живут до конца endFrame,
// даже если в этом кадре act() заменил мир (слияние шагов отмены освобождает промежуточные версии).
const World& frameWorld(App& a);

// Вид фракции: подпись, значок, тон бейджа.
struct KindInfo {
  const char* label;
  const char* icon;
  ui::Tone tone;
};
KindInfo kindInfo(const Faction& f);
std::string displayName(const Faction& f);   // название или «Без названия»

// Состояние отношений: значок, цвет из темы, подпись.
const char* relIcon(RelStatus s);
Color relColor(RelStatus s);
const char* relLabel(RelStatus s);
// Выбор состояния отношений: таблетка цвета состояния, меню со значками. true — изменено.
bool statusPicker(std::string_view id, RelStatus& s, bool disabled = false);

// Ресурс: название из справочника.
std::string resourceName(const World& w, Id res);

// Суммы денег и ресурсов одинаково во всех вкладках: дробная часть — только у малых нецелых значений.
int moneyDigits(double v);
std::string money(double v);
std::string moneySigned(double v);

// Выбор записи справочника для поля фракции: как w::catalogPicker, но «Добавить…» создаёт запись и сразу
// назначает её (одно действие, одна отмена). assign(tx, id) — записать выбранное в фракцию.
bool catalogField(App& a, std::string_view id, rules::CatalogList list, Id value, std::string_view noneLabel, const char* undoLabel,
                  std::function<void(Tx&, Id)> assign, bool disabled = false);

// Строка суммы: значок (цвет категории), подпись с подсказкой (формула), значение справа.
void moneyRow(const char* icon, Color iconColor, std::string_view label, double value, std::string_view tip, bool sign = false,
              ui::Ink valueInk = ui::Ink::Normal);
// Полоса долей (структура доходов/расходов): отрезки-таблетки цветов категорий.
struct Share {
  double value;
  Color color;
};
void shareBar(std::span<const Share> parts, float height = 6);

// Флаг в прямоугольнике (копия данных в замыкании — безопасно для временных флагов).
void drawFlagCopy(const Flag& f, RectF r, float radius);
// Невидимая подсказка поверх прямоугольника (для своих виджетов без lastItem).
void tipOver(RectF r, std::string_view key, std::string_view tip);
// Кольцо фокуса клавиатуры для своих виджетов.
void focusRing(RectF r, float radius);

// Портрет персонажа (декодированный и уменьшенный; кеш по содержимому). nullptr — нет портрета.
const gfx::Image* portraitOf(const Character& c);
// Где персонаж в войске: ID объекта (0 — нигде).
Id armyOfCharacter(const World& w, Id character);

// Новая фракция: приятный различимый цвет и флаг по умолчанию.
Color pickNewColor(const World& w);
Flag defaultFlag(FactionKind kind, Color color, Id seed);
// Создать фракцию с цветом и флагом, выделить и начать переименование. 0 — не создана.
Id createFactionUi(App& a, FactionKind kind);
// Удаление с подтверждением (опасное действие; отменяется Ctrl+Z).
void confirmDelete(App& a, Id faction);
// Государственная гильдия (ТЗ 1.d.vi) из панели государства; уведомление с переходом.
Id createStateGuildUi(App& a, Id state);
// Открыть редактор флага.
void openFlagEditor(App& a, Id faction);

// Строка списка фракций: флаг, название, подзаголовок, подсказка справа (значок и текст), выделение, фокус.
// Ввод — щелчок, двойной щелчок, правый щелчок (контекстное меню — ui::openContextMenu вызывающим).
struct RowEvents {
  bool clicked = false, doubleClicked = false, rightClicked = false;
  RectF rect;
};
RowEvents factionRow(const Faction& f, std::string_view subtitle, std::string_view hint, const char* hintIcon, bool selected,
                     float height = 52);
// Подзаголовок строки: правитель с титулом (государство) или государство расположения (гильдия).
std::string factionSubtitle(const World& w, const Faction& f);

// Переименование в шапке: запрос (после создания, F2, меню) и его получение шапкой.
void requestRename(Id faction);
bool takeRename(Id faction);

// Перейти на вкладку инспектора фракции.
void showTab(App& a, const char* tabId);

// Вкладки (ID для перехода и тестов).
constexpr const char* kTabOverview = "faction.overview";
constexpr const char* kTabCouncil = "faction.council";
constexpr const char* kTabProvinces = "faction.provinces";
constexpr const char* kTabHqs = "faction.hqs";
constexpr const char* kTabEconomy = "faction.economy";
constexpr const char* kTabDiplomacy = "faction.diplomacy";
constexpr const char* kTabHeroes = "faction.heroes";
constexpr const char* kTabModifiers = "faction.modifiers";
constexpr const char* kTabGuilds = "faction.guilds";

bool isState(App& a, Id id);
bool isGuild(App& a, Id id);

}  // namespace rg::app::fac
