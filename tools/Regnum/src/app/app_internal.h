// Regnum — внутреннее устройство оболочки приложения (только для src/app/*.cpp и тестов app).
#pragma once
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "app/app.h"
#include "core/io.h"
#include "gfx/image.h"
#include "map/basemap.h"
#include "map/mapview.h"
#include "ui/ui.h"

namespace rg::app {

// Снимок хода, ещё не записанный на диск: ходы мира без файла и ходы после последнего сохранения.
// Записываются в историю при сохранении; «Не сохранять» (смена или закрытие мира) их отбрасывает.
struct MemSnapshot {
  int turn = 0;
  World world;
  std::string label, at;
  u64 seq = 0;             // уникальный номер в истории мира
  std::string kind;        // io::kSnapStart / kSnapEnd / kSnapBranch
  u64 parent = 0;          // предыдущий снимок той же ветви (0 — нет)
  std::shared_future<std::vector<u8>> gz;   // сжатие (io::pack) в фоне — к сохранению обычно уже готово
};

struct App::Impl {
  AppConfig cfg;
  std::string dataDir;
  // ---- базовая карта и отрисовка
  std::unique_ptr<map::Basemap> basemap;
  std::unique_ptr<map::MapView> map;
  // ---- проект
  std::string path;                        // папка или .regnum; пусто — не сохранён
  bool bundle = false;
  io::FileState files;                     // отпечатки файлов папки (внешние изменения)
  u32 fixedTables = 0;                     // исправлены нормализацией — переписать при сохранении
  u32 forceTables = 0;                     // «оставить мои» при внешнем изменении — переписать с заменой
  bool overwriteExternal = false;
  std::map<std::string, i64> ackExternal;  // файл -> время, о котором уже спросили
  std::string savedAt, autosavedAt;        // ISO
  double lastAutosave = 0;                 // platform::time()
  u64 autosaveVersion = 0;                 // версия хранилища при последнем автосохранении
  std::future<std::string> autosaveJob;    // фоновая запись автосохранения (ошибка или пусто)
  bool folderAutosaveBlocked = false;      // ошибка записи в папку — до ручного сохранения
  // Будильник: разбудить цикл событий к сроку автосохранения, даже если окно простаивает.
  struct WakeTimer {
    std::thread th;
    std::mutex m;
    std::condition_variable cv;
    std::chrono::steady_clock::time_point due;
    bool armed = false, stop = false;
    void arm(double seconds);
    ~WakeTimer();
  } wakeTimer;
  std::vector<MemSnapshot> memSnaps;
  // ---- история ходов: ветвь текущего мира (снимок, от которого он идёт) и её смена при отмене
  u64 snapHead = 0, snapNext = 1;          // текущая ветвь и следующий номер снимка
  std::vector<std::pair<u64, u64>> headUndo, headRedo;   // смена ветви (до, после) для отмены и повтора
  int headSub = 0;                         // подписка на хранилище (отмена «Завершить ход» и возвратов)
  std::optional<io::History> diskHist;     // история на диске (кеш до следующего сохранения или открытия)
  io::FileStamp bundleStamp;               // отпечаток архива .regnum (внешние изменения)
  std::vector<std::string> autoFiles;      // файлы папки, версия которых на диске записана автосохранением
  std::function<void(App&)> afterSave;     // продолжение после «Сохранить как»
  // ---- кадр
  gfx::Image frameImg, mapImg;
  u64 mapKey = 0, mapGen = 1;
  float lw = 0, lh = 0, dpi = 1;           // логический размер окна и масштаб
  std::string title;
  // ---- ввод карты
  bool spaceDown = false;
  bool panning = false;
  bool toolDown = false;
  float lastMx = -1e9f, lastMy = -1e9f;    // точки интерфейса
  bool mapHovered = false;
  bool fitPending = false;                 // показать всю карту, когда известен размер окна
  // ---- элементы оболочки (логические пиксели)
  std::unordered_map<std::string, RectF> rects, rectsNext;
  // ---- экран запуска
  gfx::Image preview;                      // уменьшенная базовая карта
  bool previewTried = false;
  gfx::Image backdropBase;                 // перекрашенная размытая карта (на тему)
  bool backdropBaseDark = true;
  gfx::Image backdrop;                     // фон под размер окна
  int backdropW = 0, backdropH = 0;
  bool backdropDark = true;
  std::vector<io::RecentProject> recent;
  bool recentDirty = true;
  std::unordered_map<std::string, std::shared_ptr<gfx::Image>> thumbs;   // путь -> миниатюра (пусто — нет)
  std::vector<io::AutosaveInfo> recovery;  // автосохранения новее проектов
  bool recoveryDirty = true;
  // ---- настройки приложения (app.json)
  std::string browserDir;
  bool builtinBrowser = false;             // свой проводник и там, где есть системные диалоги
  bool prefsDirty = false;
  // ---- диалоги и отложенные действия кадра
  bool drawingDialogs = false;
  bool closeAllDialogs = false;
  std::vector<std::unique_ptr<Dialog>> pendingDialogs;
  std::vector<std::function<void(App&)>> later;
  std::string lastError;
  double lastErrorAt = -10;
};

namespace detail {

// Отложить действие до конца кадра (после сведения интерфейса): системные диалоги, открытие файлов.
void later(App& a, std::function<void(App&)> fn);
void savePrefs(App& a);

// ---- оболочка (shell.cpp, start.cpp)
void mapInput(App& a);                     // начало кадра редактора: указатель над картой
void mapWheel(App& a);                     // конец кадра: колесо над картой
void drawEditorScreen(App& a);
void drawStartScreen(App& a);
void drawDialogs(App& a);
void drawToasts(App& a, RectF area);       // уведомления в правом нижнем углу области
void globalKeys(App& a);                   // сочетания команд и инструментов (после панелей)
void renderMap(App& a, gfx::Canvas& c);    // карта (с кешем) и наложение инструмента
void renderStartBackdrop(App& a, gfx::Canvas& c);
map::RenderOptions renderOptions(App& a);

// ---- проект (project.cpp)
void autosaveTick(App& a);
void writeThumbnail(App& a);               // миниатюра политической карты для списка недавних
std::string thumbPath(const std::string& dataDir, const std::string& project);
void refreshStartData(App& a);             // недавние и автосохранения для экрана запуска
void restoreAutosave(App& a, const io::AutosaveInfo& info);

// ---- диалоги (dialogs.cpp, browser.cpp, settings.cpp, palette.cpp, turn.cpp)
std::unique_ptr<Dialog> confirmDialog(std::string title, std::string text, std::string ok, bool danger, std::function<void(App&)> onYes,
                                      std::function<void(App&)> onNo = {});
std::unique_ptr<Dialog> choiceDialog(std::string title, std::string text, std::vector<std::string> buttons,
                                     std::function<void(App&, int)> onPick, const char* icon, bool firstSubtle);
std::unique_ptr<Dialog> promptDialog(std::string title, std::string label, std::string initial,
                                     std::function<void(App&, const std::string&)> onOk);
std::unique_ptr<Dialog> messageDialog(std::string title, std::string text, std::vector<std::string> lines, ToastKind kind);
std::unique_ptr<Dialog> newWorldDialog(App& a);
std::unique_ptr<Dialog> saveAsDialog(App& a);
std::unique_ptr<Dialog> settingsDialog();
std::unique_ptr<Dialog> paletteDialog();
std::unique_ptr<Dialog> helpDialog();
std::unique_ptr<Dialog> turnConfirmDialog(App& a, rules::TurnReport preview);
std::unique_ptr<Dialog> turnReportDialog(App& a, rules::TurnReport report);
std::unique_ptr<Dialog> historyDialog(App& a);

enum class BrowseMode : u8 { OpenWorld, OpenBundle, SaveBundle, PickFolder };
std::unique_ptr<Dialog> browserDialog(BrowseMode mode, std::string title, std::string startDir,
                                      std::function<void(App&, const std::string&)> onPick, std::string defaultName = {},
                                      std::function<void(App&)> onCancel = {});
// Выбор пути: системный диалог (если есть и не выбран свой проводник) или встроенный проводник.
void pickPath(App& a, BrowseMode mode, std::string title, std::string startDir,
              std::function<void(App&, const std::string&)> onPick, std::string defaultName = {},
              std::function<void(App&)> onCancel = {});
// Папка сохранения по выбору: пустая или мир — она сама, иначе — новая вложенная по названию мира.
std::string folderTarget(App& a, const std::string& picked);
std::string newWorldFolder(const std::string& parent, const std::string& name);

// ---- утилиты (util.cpp)
ui::Shortcut parseShortcut(std::string_view s);   // «Ctrl+Shift+S», «F1», «V», «1», «+»; пусто — нет
std::string shortcutLabel(std::string_view s);    // для подсказок: «Ctrl+S» / «⌘S»
i64 isoToMs(std::string_view iso);                // «2026-10-01T12:00:00Z» -> мс с 1970; 0 — не разобрано
std::string localTime(std::string_view iso);      // «12:40» сегодня, иначе «2 окт., 12:40»
std::string sanitizeName(std::string_view name);  // имя папки из названия мира
std::optional<gfx::Image> loadImage(const std::string& path, int maxSide = 0);
std::string findBasemapDir();
std::string entityName(const World& w, Selection s);   // название выделенного объекта
const char* selIcon(SelType t);
const char* selCaption(SelType t);
Box2 selectionBox(const World& w, Selection s);       // область объекта на карте (пусто — нет)
bool selectionExists(const World& w, Selection s);

// ---- оболочка (shell.cpp)
// Значок «галочки» режима правки границ (ТЗ 1.a.ii): замок закрыт — границы закреплены, открыт — правка.
// Отличается от значка инструмента «Правка границ провинции» (tool-edit).
const char* bordersToggleIcon(bool on);

}  // namespace detail
}  // namespace rg::app
