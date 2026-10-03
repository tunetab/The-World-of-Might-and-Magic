# Regnum — архитектура

---
type: tool_documentation
status: active
canon_level: support
updated_real_date: 2026-10-02
---

Regnum — нативный редактор мира «Меча и Магии»: карта провинций, государства, гильдии, войска, экономика и ходы. Документ — контракт для всех, кто пишет код редактора (людей и агентов).

## 1. Жёсткие правила

1. **Язык — C++20, только стандартная библиотека.** Никаких сторонних библиотек, заголовков, систем сборки, скриптовых движков, веб-технологий. Всё своё: растеризатор, шрифты, интерфейс, PNG/JPEG, zlib, zip, JSON.
2. **Системные API ОС допустимы только в `src/platform/*`**: Win32/GDI (Windows), Xlib через `dlopen("libX11.so.6")` (Linux), Objective-C runtime + AppKit + CoreGraphics через `dlopen` (macOS). Остальной код не знает об ОС, кроме `base/fs.cpp` (пути) и `base/jobs.cpp` (потоки std).
3. **Одна кодовая база на три ОС.** Код вне `platform/` обязан компилироваться Clang и GCC на Windows, Linux, macOS. Нет `#ifdef _WIN32` вне `platform/` и `base/`.
4. **Мир неизменяем.** Читать — `const World&`. Менять — только `Store::transact(label, [&](Tx& tx){...})`. Исключение внутри — откат. Причина отказа для пользователя — `rg::fail("…")` (UserError, по-русски).
5. **Интерфейс по-русски, минимум текста, максимум значков.** Каждый значок-кнопка имеет всплывающую подсказку с названием и сочетанием клавиш.
6. **Никаких заглушек.** Функция либо работает до конца, либо её нет. Нет TODO в выпуске.
7. **Детерминизм.** Расчёты правил не зависят от порядка обхода хеш-таблиц, времени и потоков. Случайность — только `Rng` с явным зерном.
8. **Ошибки не роняют приложение.** Ошибка панели/инструмента показывается уведомлением, мир остаётся согласованным (транзакция откатывается).

## 2. Каталоги

```
tools/Regnum/
  build.sh               сборка (Git Bash на Windows, Linux, macOS)
  build.ps1              сборка на Windows без Git Bash
  src/
    base/   base.h (типы, UTF-8, числа по-русски), json, fs, jobs
    codec/  zlib (inflate/deflate, crc32), png, jpeg, zip, base64
    gfx/    path.h, image, raster (AA), stroke, canvas, font (TrueType), icons, emblems, flag
    platform/ platform.h, win32.cpp, x11.cpp, cocoa.cpp, headless.cpp
    core/   world.h (модель, транзакции, хранилище), schema (перечисления), io (JSON, папка проекта, zip-архив, снимки ходов)
    geo/    geom, grid, topo (грани плоского графа), ops (операции над провинциями)
    rules/  mods, calc, entities, military, diplomacy, trade, build, tech, guilds, routes, turn, log
    map/    basemap (тайлы и маска), camera, mapview (композиция слоёв), layers, modes, labels, objects, tools/*
    ui/     ui.h — собственный immediate-mode интерфейс, тема, виджеты, таблицы, всплывающие окна
    app/    main.cpp, приложение, экраны, панели, диалоги, редакторы деревьев, проект
    cli/    regnum-cli: validate, inspect, build-basemap, render
    tests/  test.h, test_main.cpp, test_<модуль>_*.cpp, сценарии интерфейса
  assets/
    source/Expanded Map.png   исходная карта 8000 × 4500
    basemap/                   сгенерированные тайлы, береговая линия, маска (regnum-cli build-basemap)
    regnum.rc, regnum.ico, regnum.manifest
  bin/<os>/                    готовые сборки
  docs/                        ARCHITECTURE.md, FORMAT.md, RULES.md
```

Зависимости модулей (build.sh): `base ← codec ← gfx`, `codec ← core ← geo ← rules`, `gfx+rules ← map`, `gfx ← ui`, всё ← `app`. Модуль не включает заголовки модулей правее себя. `platform.h` не зависит от gfx: кадр передаётся как буфер пикселей.

Тесты модуля: `src/tests/test_<модуль>_<тема>.cpp`. Цель `./build.sh test-<модуль> --test` собирает модуль с зависимостями и его тесты. Артефакты тестов (PNG) — в `.wmma/regnum-tests/` корня репозитория.

## 3. Соглашения кода

- Пространство имён `rg`, подпространства по модулю: `rg::gfx`, `rg::geo`, `rg::rules`, `rg::ui`, `rg::platform`, `rg::map`, `rg::app`, `rg::codec`, `rg::json`, `rg::fs`, `rg::jobs`.
- Строки — UTF-8 `std::string`. Пути — UTF-8; преобразование в `std::filesystem::path` только через `fs::path(const std::string&)`.
- Числа для пользователя — только `fmtNum/fmtPct/fmtSigned/nTurns`. Сравнение названий — `compareRu`, поиск — `utf8::matches`.
- Комментарии — по-русски, кратко. Имена — по-английски.
- Предупреждения компилятора `-Wall -Wextra` не допускаются в своём коде.
- Глобальное изменяемое состояние — только в `app/` и кешах с явной синхронизацией.
- Пиксели изображений `gfx::Image` — premultiplied BGRA в `u32` (байты B, G, R, A в памяти). Это формат DIB Windows, XImage и CoreGraphics (PremultipliedFirst | ByteOrder32Little).

## 4. Модель мира

`src/core/world.h` — единственный источник формы данных. Ключевое:

- `World` — набор устойчивых таблиц `Table<T>` (блоки по 64 записи под `shared_ptr<const>`), метаданных, настроек, каталогов, отношений.
- `Tx` — черновик: `tx.province(id)` возвращает изменяемую копию; `tx.add(Province{})` выдаёт ID; `tx.eraseX(id)` удаляет без очистки ссылок.
- `Store` — текущее значение, отмена/повтор (400 шагов), объединение (`TxOptions::coalesce`), `dirtyTables()` для сохранения только изменённых файлов, подписки `subscribe`.
- `World::diff(a, b)` и `Table::diff` дают точечные изменения для инвалидации кешей и тайлов.
- Снимок хода — просто `World` (копирование O(1)).

Перечисления, подписи, значки, пределы эффектов и константы ТЗ — `src/core/schema.h`. Формулы — [RULES.md](RULES.md). Формат файлов — [FORMAT.md](FORMAT.md).

## 5. Модули и их контракты

Точные сигнатуры — в заголовках модулей (они главнее этого раздела); здесь описано назначение и важные решения.

### 5.1 base (json, fs, jobs)

`base/json.h`: `json::Value` (null, bool, число double, строка, массив, объект с сохранением порядка ключей). `json::parse(text) -> Value` (строгий, ошибка с номером строки и столбца, `\u` и суррогаты), `json::write(value, {indent, sortKeys, compactNumbersArrays})`. Числа пишутся кратчайшим точным представлением (`std::to_chars`). Удобные методы: `get(key)`, `num(key, def)`, `str(key, def)`, `boolean(key, def)`, `arr(key)`, `obj(key)`, `set(key, v)`, `push(v)`.

`base/fs.h`: `readFile(path) -> optional<string>`, `writeFileAtomic(path, data)` (временный файл + замена; на Windows `MoveFileExW` с заменой), `exists`, `isDir`, `makeDirs`, `list(dir)`, `remove`, `removeAll`, `rename`, `copyFile`, `mtime`, `fileSize`, `exeDir`, `userDataDir` (Windows `%APPDATA%/Regnum`, Linux `$XDG_CONFIG_HOME/regnum` или `~/.config/regnum`, macOS `~/Library/Application Support/Regnum`), `homeDir`, `documentsDir`, `tempDir`, `join`, `parent`, `filename`, `stem`, `ext`, `path(const std::string&) -> std::filesystem::path`, `fromPath(path) -> std::string`.

`base/jobs.h`: пул потоков (`std::thread`), `jobs::parallelFor(n, fn(i))`, `jobs::submit(fn) -> std::future`, `jobs::workers()`.

### 5.2 codec

`codec/zlib.h`: `inflate(bytes, format)` (zlib/raw/gzip), `deflate(bytes, level, format)` (LZ77 + динамический Хаффман), `crc32`, `adler32`.
`codec/png.h`: `decodePng(bytes) -> RgbaImage{w, h, std::vector<u8> rgba}` (все типы цвета, 1–16 бит, палитра, tRNS, Adam7); `encodePng(RgbaImage, level)`.
`codec/jpeg.h`: `decodeJpeg(bytes) -> RgbaImage` (baseline и progressive, 4:4:4/4:2:2/4:2:0, оттенки серого, EXIF-ориентация).
`codec/zip.h`: чтение и запись zip (stored + deflate, UTF-8 имена).
`codec/base64.h`: `encode/decode`.

### 5.3 gfx

- `gfx/path.h` — `Path`, `Affine`, `Pt` (готово).
- `gfx/image.h` — `Image{w, h, std::vector<u32> px}` premultiplied BGRA; `fromRgba(RgbaImage)`, `toRgba()`, `resize` (бокс/билинейный), `crop`, `fill`.
- `gfx/raster.*` — сглаженная заливка контуров (non-zero и even-odd) с точной площадью покрытия, отсечение прямоугольником и маской.
- `gfx/stroke.*` — обводка: толщина, соединения miter/round/bevel, концы butt/round/square, пунктир.
- `gfx/canvas.h` — `Canvas(Image&)`: стек преобразований и отсечений (прямоугольник, скруглённый прямоугольник, произвольный контур), `fillPath`, `strokePath`, `fillRect`, `fillRoundRect` (радиусы углов), `strokeRoundRect`, `fillCircle`, `line`, `drawImage` (аффинно, билинейно, прозрачность), `drawImageRect`, `linear/radial` градиенты, `boxShadow(rect, radius, blur, spread, color)` с кешем, режимы смешивания normal/multiply/screen, `drawText`, `measureText`, `drawIcon`.
- `gfx/font.h` — TrueType/TTC: cmap 4/12, glyf (простые и составные), hmtx, kern; вариативные шрифты — экземпляр по умолчанию. Кеш глифов по (глиф, кегль ¼ px, субпиксельный сдвиг ¼ px). Раскладка: кернинг, перенос по словам, многоточие, выравнивание, цепочка запасных шрифтов. Отсутствующие U+202F/U+2009 — узкий пробел. Поиск системных шрифтов: Windows `%WINDIR%/Fonts` (Segoe UI, Segoe UI Semibold/Bold, Cambria/Constantia/Georgia, Consolas, Segoe UI Symbol), macOS `/System/Library/Fonts` и `/Library/Fonts` (SFNS, Helvetica Neue, Arial, Georgia, Menlo), Linux `/usr/share/fonts`, `~/.local/share/fonts`, `~/.fonts` (DejaVu, Noto, Liberation, Ubuntu, Cantarell). Шрифт с кириллицей обязателен; при отсутствии системного — сообщение об ошибке.
- `gfx/icons.h` — реестр значков: имя → контур на сетке 24 × 24, заданный строкой пути SVG (свой разборщик M/L/H/V/C/S/Q/T/A/Z, относительные команды). Стиль: линия 1,75, скруглённые концы и соединения; часть значков — заливкой. `drawIcon(canvas, name, rect, color, strokeScale)`. Неизвестное имя — заметный значок «?» и запись в журнал.
- `gfx/emblems.h` — геральдические эмблемы для флагов (заливка): корона, башня, замок, звезда, солнце, луна, дерево, лилия, меч, скрещённые мечи, щит, череп, якорь, корабль, колос, самоцвет, молот, топор, лук, ключ, глаз, пламя, орёл, лев, дракон, волк, бык, конь, змей, кракен, руна.
- `gfx/flag.h` — отрисовка `Flag` (узор + цвета + эмблема или PNG) в прямоугольник со скруглением, лёгкой тканевой тенью и рамкой.
- Фигурки карты: воин (войско) и корабль (флот) с цветом фракции — значки `fig-army`, `fig-fleet`, `fig-allied-army`, `fig-allied-fleet`.

### 5.4 platform

`platform/platform.h`:

```cpp
namespace rg::platform {
enum class Key : u16 { Unknown, A..Z, D0..D9, F1..F12, Escape, Enter, Tab, Backspace, Delete, Insert, Home, End, PageUp, PageDown,
                       Left, Right, Up, Down, Space, Minus, Equal, LBracket, RBracket, Semicolon, Quote, Comma, Period, Slash,
                       Backslash, Grave, NumPad0..NumPad9, NumAdd, NumSub, NumMul, NumDiv, NumDecimal, NumEnter, Shift, Ctrl, Alt, Super };
enum Mod : u32 { ModShift = 1, ModCtrl = 2, ModAlt = 4, ModSuper = 8 };
u32 primaryMod();                 // Ctrl на Windows/Linux, Super (⌘) на macOS
struct Event { Type type; float x, y; int button; int clicks; float wheelX, wheelY; bool precise; Key key; u32 mods; bool repeat;
               std::string text; std::vector<std::string> files; };
enum class Cursor { Arrow, Hand, IBeam, Crosshair, Move, ResizeH, ResizeV, ResizeNWSE, ResizeNESW, Grab, Grabbing, NotAllowed, Wait };
struct Frame { u32* px; int w, h, stride; float scale; };   // физические пиксели, premultiplied BGRA, непрозрачный результат
class App { public: virtual void onEvent(const Event&) = 0; virtual void onFrame(Frame&) = 0; virtual bool animating() = 0; virtual bool onCloseRequest() = 0; };
int run(App& app, const WindowConfig& cfg);   // цикл событий; кадр перерисовывается после invalidate() или пока animating()
void invalidate(); void wake();   // wake — потокобезопасно, из фоновых задач
void setCursor(Cursor); void setTitle(const std::string&); void setFullscreen(bool); bool fullscreen();
std::string clipboardText(); void setClipboardText(const std::string&);
std::optional<std::string> openFileDialog(title, filters, startDir); std::optional<std::string> saveFileDialog(...);
std::optional<std::string> pickFolderDialog(title, startDir);   // nullopt и dialogsSupported()==false → приложение показывает свой проводник
bool dialogsSupported(); void openPath(const std::string&); void openUrl(const std::string&);
void showFatal(const std::string& title, const std::string& text);
double time();
}
```

Координаты событий — логические пиксели (физические / scale). Колесо: `wheelY` в «строках» (1 щелчок = 1), у тачпада `precise = true` и пиксели. Двойной щелчок — `clicks = 2`. Перетаскивание файлов в окно — `FilesDropped`.

Реализации: `win32.cpp` (DPI v2, `WM_DPICHANGED`, DIB-секция, `WM_CHAR` с суррогатами, IME, захват мыши, `IFileOpenDialog`, буфер обмена, тёмная рамка окна, запоминание положения окна), `x11.cpp` (`dlopen` libX11, XIM/`Xutf8LookupString`, `XPutImage`, выделения CLIPBOARD/UTF8_STRING, курсоры, `Xft.dpi`), `cocoa.cpp` (`dlopen` libobjc/AppKit/CoreGraphics, класс NSView во время выполнения, Retina, NSPasteboard, NSOpenPanel/NSSavePanel), `headless.cpp` (кадры в память и сценарии для тестов и CLI).

### 5.5 geo

Плоский граф провинций в `World::nodes` и `World::edges`. Дуга — полилиния между узлами; береговые дуги (`Coast`) получены из базовой карты, граница карты — `Frame`. Стороны дуги помечены провинцией и сушей/морем. Грани графа — части провинций; провинция может состоять из нескольких граней (острова).

`geo/geom.h`: пересечение отрезков с допуском, проекция на отрезок, площадь и ориентация кольца, точка в многоугольнике с дырами (чёт-нечет), полюс недоступности (polylabel), упрощение Дугласа — Пекера с сохранением топологии, габариты.

`geo/grid.h`: пространственный хеш для отрезков и граней.

`geo/topo.h`:
```cpp
struct Face { Id province; Terrain terrain; double area; Box2 box; std::vector<std::vector<Vec2>> rings; /* [0] внешний, далее дыры */ std::vector<std::vector<HalfEdge>> ringEdges; Vec2 label; };
struct ProvinceShape { std::vector<int> faces; double area; Box2 box; Vec2 label; };
struct FaceSet {
  std::vector<Face> faces; std::unordered_map<Id, ProvinceShape> provinces;
  int locate(Vec2) const; Id provinceAt(Vec2) const; Terrain terrainAt(Vec2) const;
  std::vector<Id> provincesOnPolyline(const std::vector<Vec2>&) const;   // для торговых маршрутов
  std::vector<std::pair<Id, Id>> neighbors() const;                        // смежность провинций
};
std::shared_ptr<const FaceSet> faces(const World&);   // кеш по тождеству таблиц nodes/edges (потокобезопасно)
struct Issue { std::string code, msg; Vec2 at; };
std::vector<Issue> validate(const World&);            // пересечения, вырожденные дуги, висячие узлы, несогласованные метки
```

`geo/ops.h` — все операции внутри транзакции; ошибка — `fail("…")`:
```cpp
void initFromCoast(Tx&, const Coast&);                          // рамка + береговые кольца; всё не назначено
Id createProvince(Tx&, const std::vector<Vec2>& poly, Terrain t /*None = по первой точке*/);   // новая провинция из многоугольника, соседи уменьшаются; берег «прилипает»
void addArea(Tx&, Id province, const std::vector<Vec2>& poly);  // расширить (соседи уменьшаются)
void removeArea(Tx&, Id province, const std::vector<Vec2>& poly);  // вырезать в «не назначено»
Id fillAt(Tx&, Vec2 p, Id province /*0 = новая*/);              // назначить грань целиком (остров одним щелчком)
Id split(Tx&, Id province, const std::vector<Vec2>& line, NewProvinceFn fn = {});   // нож; возвращает новую провинцию
void merge(Tx&, Id target, Id source);                          // геометрия source → target (запись source удаляет rules)
void unassign(Tx&, Id province);                                // геометрия → «не назначено»
struct Handle { enum Kind { None, Node, Point } kind; Id edge, node; int index; };
Handle hitHandle(const World&, Vec2 p, double tol, Id province /*0 = любые*/);
std::optional<EdgeHit> hitEdge(const World&, Vec2 p, double tol);
Vec2 handlePos(const World&, const Handle&); bool handleLocked(const World&, const Handle&);
bool canMove(const World&, const Handle&, Vec2 to);             // без пересечений
void moveHandle(Tx&, const Handle&, Vec2 to);                   // общая граница двигает обе провинции
Handle insertPoint(Tx&, Id edge, int segment, Vec2 p);
void deletePoint(Tx&, const Handle&);
void slideJunction(Tx&, Id node, Vec2 to);                      // узел на берегу скользит вдоль берега
```
Береговые точки заблокированы (карта = изображение). Провинция «морская» (`Province::sea`) — флаг данных; рельеф грани (`Terrain`) определяется береговой линией и используется для прилипания и проверок войск (войско — суша, флот — море).

### 5.6 rules

Один заголовок `rules/rules.h`, реализация по файлам. Расчёты — от `const World&`, изменения — через `Tx&`. Все формулы — [RULES.md](RULES.md).

```cpp
struct EffectSource { enum Kind { Province, Faction, Tech, Building, Guild } kind; Id id; Id modifier; };
struct Effects { std::array<double, kFxCount> v{}; std::map<Id, double> diplomacy; std::vector<EffectSource> sources; double operator[](Fx f) const; };
Effects provinceEffects(const World&, Id province);
Effects factionEffects(const World&, Id faction);

struct GuildShare { Id guild; double pct; bool hq; double gross, tax, net; };
struct ProvinceCalc { bool sea; Id owner, recipient; int slotsSize, slotsCity, slotsMods, slots, slotsUsed; double tradeValue; int routes;
                      double production; double rebellion; double buildCostFactor; double taxState, taxLocal, taxTotal; i64 population;
                      std::vector<std::pair<Id, i64>> races; std::vector<GuildShare> guilds; double provinceTax, guildTax; Effects fx; };
struct RowCalc { Id row; i64 total, field, garrison, reserve; double upkeepEach, upkeepTotal; };
struct ResourceFlow { double stock, production, tradeIn, tradeOut, net; };
struct FactionCalc { std::vector<Id> provinces, hqs; i64 population; std::vector<std::pair<Id, i64>> races;
                     double incProvinces, incGuildTax, incGuilds, incTrade, incTribute, incGross, incomePct, incTotal;
                     double expArmy, expFleet, expSpecialists, expTrade, expTribute, expTotal, net, treasury;
                     std::vector<RowCalc> army, fleet; std::map<Id, ResourceFlow> resources; Effects fx; };
struct Calc { std::unordered_map<Id, ProvinceCalc> provinces; std::unordered_map<Id, FactionCalc> factions; std::unordered_map<Id, int> routeCounts; };
std::shared_ptr<const Calc> calc(const World&);   // кеш по версии мира, потокобезопасно
```

Действия (все бросают `UserError` при нарушении правил и пишут хронику, где это событие мира):
- `entities`: `createFaction(tx, kind)`, `removeFaction`, `createCharacter`, `removeCharacter`, `createModifier`, `removeModifier`, `createBuilding(tx, owner)`, `removeBuilding`, `createTech(tx, faction)`, `removeTech`, `addCatalogItem(tx, list)`, `removeCatalogItem`, `deleteProvince(tx, id)` (геометрия + запись + ссылки), `mergeProvinces(tx, target, source)`, `setProvinceOwner(tx, id, faction)`, `setOccupied(tx, id, occupier)`, `copyTechTree(tx, from, to)`.
- `military`: `createArmy(tx, kind, faction, pos)`, `setUnits(tx, army, faction, row, count)`, `setHero(tx, army, character, on)`, `setCommander`, `setGarrison(tx, province, row, count)`, `disband`, `moveArmy(tx, army, pos)`, `encounter(w, moving, target) -> Encounter{Merge, Battle, Alliance, DeclareWar, Blocked}`, `mergeArmies`, `formAllied`, `dissolveAllied`, `splitArmy(tx, army, spec, pos)`, `resolveBattle(tx, BattleResult)`, `findFreeSpot(w, kind, near, exclude)`, `deployed(w, faction)`.
- `diplomacy`: `setRelation(tx, a, b, value, status)`, `declareWar`, `relationsOf(w, faction)` — все прочие фракции.
- `trade`: `validateDeal`, `concludeDeal(tx, Deal)` (разовые позиции исполняются сразу), `cancelDeal`, `imposeTribute(tx, kind, receiver, payer, amount, turns)`.
- `build`: `options(w, province) -> вариантов с ценой и причинами недоступности`, `startBuilding(tx, province, building)`, `cancelBuilding` (полный возврат стоимости), `demolish`.
- `tech`: `canResearch`, `setStudied`, `startResearch`, `stopResearch`, `wouldCycle`, `autoLayout(tx, faction)`.
- `guilds`: `buildHq`, `removeHq`, `setInfluence`, `setHomeState`, `createStateGuild(tx, state)`.
- `routes`: `createRoute`, `setRoutePoints`, `removeRoute`.
- `log`: `addLog(tx, kind, text, refs)`.
- `turn`: `TurnReport endTurn(Tx&)`, `TurnReport previewTurn(const World&)`.

### 5.7 core/io

Чтение и запись папки проекта (раскладка — FORMAT.md), нормализация (значения по умолчанию, пределы, висячие ссылки → предупреждения), архив `.regnum` (zip той же папки), снимки ходов `history/turn-NNNN-SSSSSS.json.gz` (начало и конец хода, ветви после возврата; новые — в памяти до сохранения), резервные копии `.regnum-backup/` (ручные и автосохранения — разные очереди), обнаружение внешних изменений файлов и архива (по размеру, времени и хешу), список недавних проектов и автосохранение в `userDataDir`. Сохраняются только изменённые таблицы (`Store::dirtyTables`). JSON — с отсортированными ключами и стабильным порядком для аккуратных диффов в Git.

### 5.8 map

- `basemap.*`, `basemap_build.*`, `basemap_segment.cpp`, `basemap_coast.cpp` — загрузка и сборка базовой карты (слои ocean, inland, symbols; пирамида уровней 0–3; береговая линия; маска моря; превью).
- `mapview.h/.cpp` — камера, композиция кадра, режимы, легенда, мини-карта, попадание мышью. Внутреннее: `tiles.cpp` и `tilestore.cpp` (фоновые тайлы 512 px на текущем масштабе со всем, что под подписями), `rastercache.cpp` (декодирование PNG-тайлов, LRU 384 МБ), `style.cpp` (цвета режимов), `labels.cpp` (подписи со спрайтовым кешем), `overlay.cpp` (маршруты, диаграммы гильдий, маркеры, войска, выделение), `marks.cpp` (раскладка отметок войск и флота).
- Камера: `setSafeArea` — свободная часть окна между панелями (приложение передаёт её каждый кадр). «Показать всю карту» вписывает мир в неё, `minZoom` — масштаб, при котором весь мир виден в ней; смена свободной части (открылся инспектор) камеру не сдвигает.
- Войска и флот не наслаиваются на экране: отметки, которые при текущем масштабе пересеклись бы (фигурка, значок численности, с зазором), сливаются в стопку — верхний объект (выделенный, иначе самый многочисленный), за ним до двух фигурок других объектов цветами их фракций, золотой значок с числом объектов, численность — сумма. Раскладка не зависит от сдвига камеры; при приближении стопка распадается. `armyAt` возвращает верхний объект, `markAt`/`armyMarks` — отметки, `separateZoom` — масштаб, при котором стопка распадётся (щелчок по стопке инструментом «Выбор» приближает к нему, перетаскивание переносит верхний объект).
- Торговый маршрут рисуется ровно по своей ломаной (изломы скруглены обводкой) — по той же линии считаются его провинции и бонус +10 % (`geo::provincesOnPolyline`).
- `demo_world.*` — вымышленный демонстрационный мир для тестов и показа (не канон кампании).
- Порядок слоёв: белая суша → заливка провинций → море → реки и озёра → заливка сухопутных провинций (без галочки «Морская») на морской части карты, полупрозрачно → границы, штриховка оккупации → символы карты → подписи → маршруты, диаграммы гильдий, штабы, столицы → войска и флот → выделение и инструменты.
- Инвалидация точечная: после изменения мира перерисовываются только тайлы, задетые изменёнными дугами и провинциями; старые тайлы видны до готовности новых.

### 5.9 ui

Собственный immediate-mode интерфейс поверх `gfx::Canvas`. Требования:

- Устойчивые ID виджетов (хеш строки + стек областей), горячий/активный элемент, фокус клавиатуры, Tab/Shift+Tab, Enter/Esc.
- Раскладка: строки/столбцы с отступами и промежутками, фиксированные и «гибкие» размеры, прокручиваемые области с инерцией, виртуализация длинных списков, разделители с перетаскиванием.
- Слои: основной, всплывающие (выпадающие списки, контекстные меню), подсказки, модальные окна, уведомления, перетаскиваемый объект.
- Виджеты: кнопка (основная, вторичная, призрачная, опасная), кнопка-значок, сегментный переключатель, переключатель, флажок, радио, ползунок, числовое поле (перетаскивание по подписи, колесо, ввод с проверкой пределов, единицы), текстовое поле (однострочное и многострочное, выделение, буфер обмена, отмена в поле, поиск слов), выпадающий список с поиском, выбор цвета (палитра + HSV + hex), вкладки-значки, таблица (сортировка, правка ячеек, добавление/удаление строк, итоговая строка, закреплённая шапка), дерево, значки-чипы фракций/провинций/ресурсов, полоса прогресса, индикатор (−100…100), круговая диаграмма, бейджи, пустые состояния, уведомления, подтверждения.
- Анимации: плавные переходы наведения (120 мс), выезд панелей (180 мс), без дёрганий; при отсутствии анимаций кадр не перерисовывается.
- Масштаб интерфейса: системный DPI × пользовательский множитель (90–150 %).

### 5.10 app

Реестры `app.h`: TabReg (вкладки инспектора), HeaderReg (шапка инспектора), DrawerReg (левая лента), EditorReg (полноэкранные редакторы), ToolReg (инструменты карты), CommandReg (команды и сочетания), DialogReg (модальные окна по ID). Общие виджеты предметной области — `app/widgets.h`. Панели: `app/panels/*` (провинция, фракции, войска, персонажи, хроника, торговля, постройки, технологии, маршрут), диалоги `app/dialogs/*`, редакторы `app/editors/*`, инструменты `app/tools*.cpp`.


Экран запуска (новый мир, открыть папку, открыть архив, недавние), основной экран (карта на весь экран, верхняя панель с режимами карты, ходом и сохранением, левая панель-лента разделов, плавающая панель инструментов, правый инспектор), панели провинции/государства/гильдии/войска/персонажа, списки, диалоги (битва, встреча войск, разделение, торговля, дань, выборщики, флаг, итог хода, история ходов, хроника, настройки, проводник файлов), полноэкранные редакторы (дерево технологий, дерево построек, модификаторы, справочники), палитра команд Ctrl+K, справка по клавишам F1.

## 6. Дизайн

Тёмная тема по умолчанию, светлая — по выбору. Ощущение: дорогой стратегический редактор, спокойный графит и золото, ничего лишнего.

| Токен | Тёмная | Светлая |
| --- | --- | --- |
| bg | #0e1117 | #f4f1ea |
| surface1 (панели) | #151a22 | #fbf9f4 |
| surface2 (карточки) | #1b2129 | #ffffff |
| surface3 (поля, наведение) | #242b36 | #efeae0 |
| border | #2b3340 | #ddd5c6 |
| borderStrong | #3a4454 | #c9bea9 |
| text | #ece8df | #1d1a14 |
| textDim | #a7afbb | #5d574c |
| textMuted | #6f7886 | #8f877a |
| accent (золото) | #d9a441 | #b07d1f |
| accentHover | #e8b75c | #c48f2c |
| onAccent | #1a1408 | #ffffff |
| success | #4fae6d | #2f8a4c |
| warning | #e0a33a | #b7791f |
| danger | #e05a4f | #c2392f |
| info | #4a9fe0 | #2f7fc0 |

Радиусы: поля 7, карточки 10, панели 14, таблетки — полный. Тени панелей: 0 14 36 rgba(0,0,0,.45) (тёмная). Отступы: 4/8/12/16/24/32. Шрифты: интерфейс — системный без засечек (13 px основной, 12 px вторичный, 15/18/24 заголовки); названия государств и заголовки разделов — системный шрифт с засечками (Cambria/Georgia/DejaVu Serif). Значки — линия 1,75 на сетке 24, размер 18–20 px. Фокус клавиатуры — золотое кольцо 2 px.

Принципы: значок вместо слова, подсказка на каждом значке; числа — крупно, подписи — мелко; опасные действия — красным и с подтверждением; пустое состояние объясняет, что сделать (одна строка + кнопка); ни одного «технического» текста для пользователя; всё, что меняет мир, — отменяется Ctrl+Z.

## 7. Проверки

- `./build.sh --test` — все модульные тесты (ядро, геометрия с фаззингом, правила, кодеки, растеризация с эталонными PNG, интерфейс в headless-режиме).
- Сценарии интерфейса (`src/tests/test_app_*.cpp`) — настоящие обработчики событий на `headless`-платформе, снимки экрана PNG и проверки состояния мира.
- `regnum-cli validate <папка мира>` — целостность данных и геометрии; используется в `tools/Проверить_проект.ps1`.
