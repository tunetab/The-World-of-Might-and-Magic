// Regnum — каркас приложения: состояние интерфейса, реестры расширения, действия, проект.
//
// Каждая панель, диалог, редактор и инструмент живёт в своём .cpp и регистрируется статическим объектом
// (TabReg, HeaderReg, DrawerReg, EditorReg, ToolReg, CommandReg, DialogReg). Оболочка (app/shell.cpp) рисует
// зарегистрированное. Панели читают мир через app.world() и меняют его только через app.act():
//
//   static void drawOverview(App& a, Id pid) {
//     const Province* p = a.world().province(pid);
//     if (!p) return;
//     std::string name = p->name;
//     if (ui::textField("name", name)) a.act("Переименовать провинцию", [&](Tx& tx) { tx.province(pid).name = name; });
//   }
//   static TabReg reg({"province.overview", "info", "Обзор", 10, SelType::Province, nullptr, drawOverview});
//
// Правила для авторов панелей:
//  - Все функции App — только из главного потока (из колбэков draw/run/инструментов).
//  - Ошибка правил внутри act() — rg::fail("…"): транзакция откатывается, пользователь видит предупреждение.
//  - При просмотре прошлого хода (readOnly()) act() ничего не меняет и сообщает об этом сам.
//  - Значение из транзакции получают захватом: Id nid = 0; a.act("…", [&](Tx& tx) { nid = rules::createArmy(...); });
//  - Занятые оболочкой клавиши: V, H (инструменты), E (правка границ), F (показать выделенное), 1–9 (режимы карты),
//    M (мини-карта), L (легенда), Home, +/− (масштаб), Пробел (панорама), Esc, F1, F11,
//    Ctrl+K/N/O/S/Shift+S/Z/Y/Shift+Z/Enter/Q/W/, (запятая). Ctrl+1…9 — свободны для выдвижных панелей (DrawerDef::shortcut).
//  - Колбэки реестров вызываются внутри кадра интерфейса; исключение из них превращается в уведомление.
//  - Сценарные тесты оболочки — src/tests/test_app_util.h (apptest::Harness: настоящий App на headless-платформе).
#pragma once
#include <deque>
#include <functional>
#include <map>
#include <memory>

#include "core/schema.h"
#include "core/world.h"
#include "platform/platform.h"
#include "rules/rules.h"
#include "ui/ui.h"

namespace rg::map {
class MapView;
class Basemap;
struct View;
struct RenderOptions;
}  // namespace rg::map
namespace rg::gfx {
class Canvas;
}

namespace rg::app {

class App;

// ---------------------------------------------------------------- выделение
enum class SelType : u8 { None, Province, Faction, Army, Route, Character };
struct Selection {
  SelType type = SelType::None;
  Id id = 0;
  bool operator==(const Selection&) const = default;
  explicit operator bool() const { return type != SelType::None && id != 0; }
};

// ---------------------------------------------------------------- инструменты карты
enum class ToolId : u8 {
  Select,        // выбор: щелчок по провинции — инспектор (ТЗ 1.a.iv при выключенной правке)
  Pan,           // перемещение карты
  EditBorders,   // ручки границ выбранной провинции (ТЗ 1.a.iv при включённой правке)
  NewProvince,   // многоугольник новой провинции
  AddArea,       // расширить выбранную провинцию
  RemoveArea,    // вырезать часть выбранной провинции
  Fill,          // назначить грань (остров) щелчком
  Knife,         // разрезать провинцию линией
  Merge,         // объединить с соседней
  DeleteProvince,
  NewArmy, NewFleet,
  Route,         // торговый маршрут
  Count
};

// Событие указателя для инструмента (координаты карты и экрана).
struct PointerEvent {
  Vec2 map;              // координаты карты (пиксели исходного изображения 8000 × 4500)
  float sx = 0, sy = 0;  // логические пиксели окна (как map::View::toScreen)
  int button = 0;        // 0 левая, 1 правая, 2 средняя
  int clicks = 1;        // 2 — двойной щелчок (только pointerDown)
  u32 mods = 0;          // platform::Mod*
};

// Инструмент карты. Живёт, пока выбран (создаётся ToolDef::make при выборе, удаляется при смене).
// Указатель над картой (не над панелью) приходит сюда; средняя кнопка и Пробел+перетаскивание
// двигают карту без участия инструмента; колесо — масштаб.
class MapTool {
 public:
  virtual ~MapTool() = default;
  virtual void activate(App&) {}
  virtual void deactivate(App&) {}
  // Нажатие над картой. true — событие обработано (иначе встроенная реакция: двойной щелчок — показать объект).
  virtual bool pointerDown(App&, const PointerEvent&) { return false; }
  // Перемещение: и с зажатой кнопкой (после pointerDown), и без неё (наведение).
  virtual bool pointerMove(App&, const PointerEvent&) { return false; }
  virtual bool pointerUp(App&, const PointerEvent&) { return false; }
  // Клавиша, не поглощённая интерфейсом (Enter, Esc, Backspace, Delete, буквы...). true — поглощена.
  // Вызывается раньше глобальных сочетаний без Ctrl и раньше «Esc — снять выделение».
  virtual bool key(App&, const platform::Event&) { return false; }
  // Рисование поверх карты: холст с масштабом логических пикселей окна (как PointerEvent::sx/sy);
  // view переводит координаты карты в экранные (view.toScreen).
  virtual void drawOverlay(App&, gfx::Canvas&, const map::View&) {}
  // Поправить параметры отрисовки карты (например, спрятать перетаскиваемое войско — hideArmies).
  virtual void renderOptions(App&, map::RenderOptions&) {}
  virtual platform::Cursor cursor(App&) { return platform::Cursor::Arrow; }
  virtual const char* hint(App&) { return nullptr; }   // подсказка в строке состояния
  // true — инструменту нужен следующий кадр (анимация предпросмотра).
  virtual bool animating(App&) { return false; }
};

// ---------------------------------------------------------------- реестры
// Вкладка инспектора. Видимые вкладки типа выделения идут слева направо по order.
struct TabDef {
  const char* id;                      // "province.economy"
  const char* icon;                    // значок вкладки
  const char* title;                   // подсказка
  int order;                           // порядок слева направо
  SelType type;                        // для какого инспектора
  bool (*visible)(App&, Id);           // nullptr — всегда (например, вкладки только для гильдий)
  void (*draw)(App&, Id);              // тело вкладки (ui:: внутри прокручиваемой области инспектора)
  int (*badge)(App&, Id) = nullptr;    // число на значке (> 0), −1 — точка; nullptr — нет
};
struct TabReg { explicit TabReg(const TabDef& d); };
const std::vector<TabDef>& tabs();

// Шапка инспектора над полосой вкладок (название, флаг, фишки владельца и т. п.). Рисуется в левой части
// строки заголовка; справа оболочка ставит кнопки «показать на карте» и «закрыть». Без шапки для типа
// оболочка показывает название сущности и её вид. Несколько шапок одного типа — по order.
struct HeaderDef {
  const char* id;                      // "province.header"
  SelType type;
  int order;
  void (*draw)(App&, Id);
};
struct HeaderReg { explicit HeaderReg(const HeaderDef& d); };
const std::vector<HeaderDef>& headers();

struct DrawerDef {                     // левая выдвижная панель (списки, хроника и т. п.)
  const char* id;
  const char* icon;
  const char* title;
  int order;
  void (*draw)(App&);                  // тело (ui:: внутри прокручиваемой области панели)
  const char* shortcut = nullptr;      // «Ctrl+1»; nullptr — нет
};
struct DrawerReg { explicit DrawerReg(const DrawerDef& d); };
const std::vector<DrawerDef>& drawers();

struct EditorDef {                     // полноэкранный редактор (дерево технологий, построек, модификаторы, справочники)
  const char* id;
  const char* title;
  void (*draw)(App&, Id arg);          // arg — например, фракция; ui:: внутри области редактора под его шапкой
  const char* icon = nullptr;          // значок в шапке
};
struct EditorReg { explicit EditorReg(const EditorDef& d); };
const EditorDef* findEditor(std::string_view id);
const std::vector<EditorDef>& editors();

struct ToolDef {
  ToolId id;
  const char* icon;
  const char* title;
  const char* shortcut;                // «V», «K», «Shift+P»; пусто или nullptr — нет
  bool editMode;                       // доступен только при включённой правке границ
  std::function<std::unique_ptr<MapTool>()> make;
  int order = 100;                     // порядок на панели инструментов
};
struct ToolReg { explicit ToolReg(const ToolDef& d); };
const ToolDef* findTool(ToolId id);
const std::vector<ToolDef>& toolDefs();

struct CommandDef {
  const char* id;                      // "file.save"
  const char* title;                   // в палитре команд и справке
  const char* icon;
  const char* shortcut;                // «Ctrl+S», «Ctrl+Shift+Z», «F1»; Ctrl = ⌘ на macOS; nullptr — нет
  void (*run)(App&);
  bool (*enabled)(App&);               // nullptr — всегда
  bool global = false;                 // срабатывает и при фокусе в текстовом поле
  const char* group = nullptr;         // раздел справки F1 («Файл», «Карта»...); nullptr — «Общие»
};
struct CommandReg { explicit CommandReg(const CommandDef& d); };
const std::vector<CommandDef>& commands();
const CommandDef* findCommand(std::string_view id);
bool runCommand(App& app, std::string_view id);   // false — нет такой или недоступна

// Модальное окно: объект живёт в стеке приложения, рисуется каждый кадр, хранит своё состояние.
// Оболочка открывает ui::beginModal(id(), modal()) и вызывает draw() внутри; Esc, крестик или
// щелчок по фону (если разрешён) закрывают окно и вызывают dismissed().
class Dialog {
 public:
  struct Style {
    std::string title;
    const char* icon = nullptr;
    ui::Tone tone = ui::Tone::Accent;  // цвет значка заголовка
    float width = 440;
    bool closeButton = true;
    bool dismissOnBackdrop = false;
  };
  virtual ~Dialog() = default;
  virtual const char* id() const = 0;  // устойчивый ID модального окна (уникальный в стеке)
  virtual Style style(App&) { return {}; }
  // Нарисовать содержимое (внутри модального окна). Вернуть false — закрыть.
  virtual bool draw(App&) = 0;
  virtual void dismissed(App&) {}      // закрыто Esc/крестиком без выбора действия
};

// Фабрика диалога по имени (расширение: отчёт о ходе «turn.report», история ходов «turn.history» и т. п.).
// Зарегистрированная фабрика заменяет встроенный диалог с тем же ID.
struct DialogDef {
  const char* id;
  std::unique_ptr<Dialog> (*make)(App&, Id arg);
};
struct DialogReg { explicit DialogReg(const DialogDef& d); };
const DialogDef* findDialog(std::string_view id);

enum class ToastKind : u8 { Info, Success, Warning, Danger };
struct Toast {
  u64 id = 0;
  ToastKind kind = ToastKind::Info;
  std::string text;
  std::string icon;                    // пусто — по виду
  double start = 0, duration = 4;      // время появления и длительность (с), наведение — пауза
  double paused = 0, closeAt = -1;
  std::string actionLabel;             // кнопка в уведомлении («Отчёт»)
  std::function<void(App&)> action;
  bool hovered = false;
};

// ---------------------------------------------------------------- состояние интерфейса
enum class Screen : u8 { Start, Editor };
struct UiState {
  Screen screen = Screen::Start;
  Selection sel, hover;                    // hover — объект под указателем на карте
  ToolId tool = ToolId::Select;
  bool editBorders = false;                // ТЗ 1.a.ii — «галочка» режима правки границ
  schema::MapMode mapMode = schema::MapMode::Political;
  std::string drawer;                      // открытая левая панель ("" — нет)
  std::map<SelType, std::string> tabOf;    // активная вкладка инспектора по типу
  std::string editor;                      // открытый полноэкранный редактор ("" — нет)
  Id editorArg = 0;
  std::optional<int> viewTurn;             // просмотр снимка прошлого хода (только чтение)
  bool darkTheme = true;
  float uiScale = 1.0f;                    // пользовательский множитель 0,9…1,5
  bool showMinimap = true;
  bool showLegend = true;
  float drawerWidth = 340;                 // точки интерфейса
  float inspectorWidth = 400;
  std::optional<Vec2> cursorMap;           // точка карты под указателем (строка состояния)
};

// Снимок хода в истории проекта (или в памяти для несохранённого мира).
struct TurnSnapshot {
  int turn = 0;
  std::string label, at;                   // подпись и время создания (ISO)
  bool memory = false;                     // ещё не записан на диск (запишется при сохранении мира)
  std::string key;                         // уникальный ключ снимка: файл истории или «mem:<номер>»
  std::string kind;                        // io::kSnapStart / kSnapEnd / kSnapBranch; пусто — старый снимок
};

struct AppConfig {
  std::string dataDir;                     // данные пользователя (пусто — fs::userDataDir())
  std::string basemapDir;                  // базовая карта (пусто — поиск assets/basemap рядом с программой и в tools/Regnum)
  bool savePrefs = true;                   // запоминать тему, масштаб, ширину панелей (dataDir/app.json)
};

// ---------------------------------------------------------------- приложение
class App : public platform::App {
 public:
  explicit App(const AppConfig& cfg = {});
  ~App() override;
  App(const App&) = delete;
  App& operator=(const App&) = delete;

  // ---- мир ----
  Store store;
  // Мир для отображения: снимок прошлого хода при просмотре истории, иначе текущий.
  const World& world() const;
  bool readOnly() const { return ui.viewTurn.has_value(); }
  // Изменение мира. UserError — предупреждение, прочее — ошибка (с записью в журнал); изменения откатываются.
  // Возвращает успех. При readOnly() — отказ с подсказкой.
  bool act(std::string_view label, const std::function<void(Tx&)>& fn, const TxOptions& opt = {});
  void undo();                             // Ctrl+Z (с подсказкой «Отменено: …»)
  void redo();                             // Ctrl+Y, Ctrl+Shift+Z
  bool dirty() const;                      // есть несохранённые изменения

  // ---- интерфейс ----
  UiState ui;
  void select(Selection s, bool focusMap = false);
  void select(SelType t, Id id, bool focusMap = false) { select(Selection{t, id}, focusMap); }
  void clearSelection() { select(Selection{}); }
  void setTool(ToolId t);                  // инструмент правки при выключенной правке границ — отказ с подсказкой
  void setEditBorders(bool on);            // переключает доступные инструменты
  void setMapMode(schema::MapMode m);
  void openDrawer(std::string_view id);    // повторный вызов закрывает
  void openEditor(std::string_view id, Id arg = 0);
  void closeEditor();
  void openDialog(std::unique_ptr<Dialog> d);
  bool openDialog(std::string_view id, Id arg = 0);   // по реестру DialogReg (или встроенный); false — нет такого
  void closeDialogs();
  bool hasDialog() const { return !dialogs_.empty(); }
  bool hasDialog(std::string_view id) const;
  // Уведомление. С actionLabel — кнопка действия (выполняется в контексте кадра).
  void toast(std::string text, ToastKind kind = ToastKind::Info, std::string icon = {}, std::string actionLabel = {},
             std::function<void(App&)> action = {});
  void error(const std::exception& e);     // UserError — предупреждение, прочее — ошибка с записью в журнал
  // Подтверждение: при согласии вызывается onYes (в контексте кадра).
  void confirm(std::string title, std::string text, std::string okLabel, bool danger, std::function<void(App&)> onYes);
  // Выбор из нескольких действий (кнопки слева направо, последняя — основная, Enter); onPick(индекс), −1 — закрыто
  // без выбора. firstSubtle — первая кнопка приглушённая (например, «Не сохранять»).
  void choose(std::string title, std::string text, std::vector<std::string> buttons, std::function<void(App&, int)> onPick,
              const char* icon = "help", bool firstSubtle = false);
  // Запрос строки: onOk(значение); пустая строка не принимается.
  void prompt(std::string title, std::string label, std::string initial, std::function<void(App&, const std::string&)> onOk);
  // Сообщение со списком строк (предупреждения при чтении мира и т. п.).
  void message(std::string title, std::string text, std::vector<std::string> lines = {}, ToastKind kind = ToastKind::Info);
  void focusMap(Box2 box);                 // плавно показать область карты (в видимой части между панелями)
  void focusMap(Vec2 p, double zoom = 0);  // 0 — оставить масштаб
  void focusSelection();                   // показать выделенный объект
  void requestRedraw();
  void setTheme(bool dark);
  void setUiScale(float s);
  void toggleFullscreen();
  void showPalette();                      // Ctrl+K
  void showHelp();                         // F1
  void showSettings();
  RectF mapArea() const;                   // часть окна между панелями (логические пиксели)
  // Прямоугольник элемента оболочки прошлого кадра (логические пиксели окна) по имени: «topbar.endturn»,
  // «tool.select», «tool.borders», «mode.3», «start.new», «inspector» и т. д. (тесты, обучение).
  const RectF* uiRect(std::string_view name) const;
  void markUi(std::string_view name);      // запомнить прямоугольник последнего элемента ui под именем
  void markUi(std::string_view name, RectF r);   // то же для прямоугольника в точках интерфейса (панели)

  // ---- проект ----
  const std::string& projectPath() const;  // папка или .regnum; пусто — мир ещё не сохранён
  bool projectIsBundle() const;
  std::string worldTitle() const;          // название мира (для заголовка окна)
  bool save();                             // Ctrl+S; новый мир — «Сохранить как»
  void saveAs();                           // Ctrl+Shift+S: папка или архив .regnum
  bool saveTo(const std::string& path);    // без вопросов: папка или .regnum (по расширению)
  void openWorldDialog();                  // Ctrl+O: выбрать папку мира
  void openBundleDialog();                 // выбрать архив .regnum
  void newWorldDialog();                   // Ctrl+N: название и папка
  // Новый мир по береговой линии базовой карты. folder непусто — сразу сохранить туда (папка создаётся).
  bool newWorld(const std::string& name, const std::string& folder = {});
  void openPath(const std::string& path);  // с вопросом о несохранённых изменениях
  bool loadProject(const std::string& path);   // без вопросов: папка, world.json или .regnum
  void loadWorld(World w, std::string_view label);   // мир без файла (демонстрация, импорт)
  void closeWorld();                       // к экрану запуска (с вопросом о несохранённых изменениях)
  void requestQuit();                      // выход с вопросом о несохранённых изменениях
  void checkExternalChanges();             // при возврате фокуса окну
  void autosaveNow();                      // автосохранение в dataDir (и в папку проекта по настройке)
  std::string dataDir() const;             // папка данных пользователя

  // ---- ход ----
  void endTurn();                          // подтверждение с итогом, затем endTurnNow
  bool endTurnNow();                       // снимок, расчёт, уведомление с отчётом
  const std::optional<rules::TurnReport>& lastTurnReport() const { return lastReport_; }
  std::vector<TurnSnapshot> snapshots() const;   // по возрастанию хода
  bool viewTurn(int turn);                 // открыть снимок только для чтения
  void backToCurrent();                    // вернуться к текущему ходу

  // ---- карта ----
  map::MapView& map();
  const map::Basemap* basemap() const;     // nullptr — базовая карта не найдена

  // ---- platform::App ----
  void onEvent(const platform::Event& e) override;
  void onFrame(platform::Frame& f) override;
  bool animating() override;
  bool onCloseRequest() override;

  // ---- служебное (оболочка, тесты) ----
  std::vector<std::unique_ptr<Dialog>>& dialogStack() { return dialogs_; }
  std::deque<Toast>& toasts() { return toasts_; }
  MapTool* activeTool() { return tool_.get(); }
  double time() const { return time_; }
  void waitBackground();                   // дождаться фонового автосохранения
  u64 frameCount() const { return frames_; }
  struct Impl;                             // внутреннее состояние (app/*.cpp)
  Impl& impl() { return *d_; }

 private:
  void onStoreChange(const Change& c);
  void buildFrame();

  std::vector<std::unique_ptr<Dialog>> dialogs_;
  std::deque<Toast> toasts_;
  std::unique_ptr<MapTool> tool_;
  std::unique_ptr<Impl> d_;
  World viewWorld_;                        // снимок для просмотра истории
  std::optional<rules::TurnReport> lastReport_;
  double time_ = 0;
  u64 toastSeq_ = 0;
  u64 frames_ = 0;
  int storeSub_ = 0;
  friend struct Shell;
};

App& app();          // текущий экземпляр (создаётся в main или тестом)
bool hasApp();

}  // namespace rg::app
