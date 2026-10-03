# Regnum — интерфейс (`src/ui/ui.h`)

---
type: tool_documentation
status: active
canon_level: support
updated_real_date: 2026-10-02
---

Собственный immediate-mode интерфейс редактора поверх `gfx::Canvas`. Каждый кадр панель описывается заново вызовами функций; всё, что должно жить между кадрами (фокус, прокрутка, раскрытие разделов, анимации, буфер правки поля, сортировка таблиц), хранится внутри по устойчивому ID. Один заголовок — `ui/ui.h`; внутренности (`ui_internal.h`, `*.cpp`) панелям не нужны.

Документ — контракт для авторов панелей и диалогов. Примеры компилируются как есть (`using namespace rg;`).

## 1. Подключение к приложению

```cpp
ui::init();                                                   // шрифты и тема; один раз
ui::setClipboard([] { return platform::clipboardText(); },
                 [](const std::string& s) { platform::setClipboardText(s); });

void App::onEvent(const platform::Event& e) { ui::onEvent(e); /* карта — если !ui::wantsMouse() */ }

void App::onFrame(platform::Frame& f) {
  gfx::Image& img = ...;                                      // обёртка над f.px
  gfx::Canvas canvas(img);
  canvas.clear(ui::theme().bg);                               // или карта на весь экран
  ui::beginFrame(f.logicalW(), f.logicalH(), f.scale, platform::time());
  shell.draw();                                               // панели, диалоги
  if (ui::shortcut({Key::S, ui::ModPrimary})) save();         // глобальные сочетания — после панелей
  ui::endFrame(canvas);
  platform::setCursor(ui::cursor());
  if (auto r = ui::textInputRect()) platform::setTextInputRect(*r);
}
bool App::animating() { return ui::needsRedraw(); }
```

- `ui::onEvent` только ставит событие в очередь; разбор — в `beginFrame`. События разносятся по кадрам: нажатие в новой точке ждёт кадр (наведение успевает обновиться), Tab/Enter/Esc завершают кадр. Поэтому щелчок занимает 2–3 кадра, а `needsRedraw()` остаётся `true`, пока очередь не пуста (`ui::pendingEvents()`).
- `wantsMouse()` — указатель над панелью, всплывающим окном, виджетом, идёт перетаскивание или открыто модальное/всплывающее окно: карта ввод не берёт.
- `wantsKeyboard()` — фокус в текстовом поле: сочетания без Ctrl/Alt/⌘ не срабатывают.
- `needsRedraw()` — идут анимации (120–180 мс), ждёт подсказка (350 мс), мигает каретка (10 с после ввода), есть уведомления, очередь не пуста. Без этого кадр не рисуется: в покое процессор свободен.
- `endFrame` холст не очищает: сначала приложение рисует фон или карту, потом интерфейс.

## 2. Единицы, масштаб, тема

- Все прямоугольники — **точки интерфейса**: логические пиксели окна / `uiScale`. Холст в `endFrame` — физические пиксели; итоговый множитель `deviceScale() = dpi × uiScale`. Прямоугольники и линии привязываются к пикселям устройства, текст и значки растеризуются под масштаб — чётко на 100/125/150 %.
- `ui::setUiScale(0.9…1.5)` — пользовательский множитель, `ui::setTheme(dark)` — тёмная (по умолчанию) или светлая тема.
- Токены — `ui::theme()`: `bg, surface1 (панели), surface2 (карточки), surface3 (поля, наведение), border, borderStrong, text, textDim, textMuted, accent (золото), accentHover, onAccent, success, warning, danger, info` и производные (`shadow, scrim, hover, pressed, selection, stripe, track`). Размеры: высота поля 30 (малое 24), значок 18 (малый 16), радиусы 7/10/14, отступы `ui::sp::xs…xxl` = 4/8/12/16/24/32.
- **Цвет — только из темы**: `ui::Ink` для текста (`Normal, Dim, Muted, Accent, Success, Warning, Danger, Info, OnAccent`), `ui::Tone` для смысла (`Neutral, Accent, Success, Warning, Danger, Info`), `ui::inkColor/toneColor`. Свои цвета — только для сущностей мира (цвет фракции, ресурса) через поле `color`.
- Роли шрифтов `ui::Font`: `Caption 11, Small 12, Body 13, Strong 13 полужирный, Subtitle 15, Title 18, Heading 22 (с засечками, жирный), Display 28 (с засечками), Mono 12, Number 20 (крупные числа)`. `ui::measure(text, font)`, `ui::lineHeight(font)`.

## 3. ID

- ID виджета — хеш подписи вместе со стеком областей. Одинаковые подписи в одной области — ошибка: в журнал пишется предупреждение, второй виджет ведёт себя непредсказуемо.
- Разделить: `"Удалить##row7"` (видно «Удалить»), `ui::IdScope s{i};` (RAII; фигурные скобки — `IdScope s(i64(x))` компилятор прочтёт как объявление функции), `ui::pushId/popId`.
- Панель, всплывающее и модальное окно — сами области ID: одинаковые кнопки в разных панелях не конфликтуют. Строки таблицы и виртуального списка получают область автоматически (индекс данных).
- `ui::id("name")` — ID в текущей области (для `setKeyboardFocus`, собственных виджетов, `state<T>`).

## 4. Раскладка

Поток контейнера вертикальный: каждый виджет берёт следующий слот, между слотами `theme().gap` (8). Поля, списки, полосы, таблицы занимают всю ширину; кнопки, флажки, бейджи — свою естественную ширину.

```cpp
{
  ui::Panel p("inspector", {W - 392, 76, 380, H - 88});          // поверхность, тень, область ID, перекрывает ввод под собой
  ui::label("Эльвенмор", {.font = ui::Font::Display});
  {
    ui::Row r({ui::px(90), ui::fr(1), ui::fr(2)}, 30, 8);     // столбцы: точки и доли; после последней ячейки — новая строка
    ui::label("Налог");  ui::numberField("tax", tax);  ui::slider("tax2", tax, 0, 60);
  }
  {
    ui::HStack hs(30, ui::Align::Left, 6);                    // по естественной ширине
    ui::iconButton("undo", "Отменить");
    ui::flex();                                               // всё дальше — к правому краю
    ui::button("Применить", {.variant = ui::Variant::Primary});
  }
  ui::Scroll sc("body");                                      // высота 0 — весь остаток панели
  ...
}
```

| Конструкция | Что делает |
| --- | --- |
| `ui::Panel p(id, rect, {.pad, .glass})` | плавающая панель: surface1, радиус 14, мягкая тень; `glass` — размытие карты под ней |
| `ui::Area a(rect, pad)` | контейнер в явном прямоугольнике без фона (ячейка разделителя, своя область) |
| `ui::Card c({.icon, .title, .tone})` | карточка в потоке, высота по содержимому; `tone` — цветная полоска слева; после закрытия карточка — `lastItem()` (подсказка, цель перетаскивания) |
| `ui::Section s(title, icon, {.defaultOpen, .badge, .card, .actionIcon, .actionTooltip})` | сворачиваемый раздел (`if (s) {...}`), плавное раскрытие, состояние помнится; кнопка в заголовке — `s.action()` («+» — добавить строку) |
| `ui::Row r(cols, height, gap)` | столбцы `px(n)`/`fr(k, min)`; `height` 0 — 30, `ui::kAuto` — по самому высокому |
| `ui::HStack hs(h, align, gap)` + `ui::flex()` | ряд по естественной ширине; `Align::Right/Center` |
| `ui::Group g(width, gap)` | вертикальная группа в одном слоте (несколько строк в ячейке) |
| `ui::Scroll sc(id, height, {.horizontal})` | прокрутка: колесо (плавно), тачпад, тонкая полоса (шире при наведении, прячется), перетаскивание, щелчок по дорожке, затухание у краёв |
| `ui::VirtualList vl(id, count, rowH, height)` | `for (int i : vl) {...}` — только видимые строки |
| `ui::splitter(id, area, pos, axis, minA, minB)` | две области и перетаскиваемая граница → `{a, b, changed}` |
| `ui::Indent i(16)`, `ui::Disabled d(cond)` | отступ; всё внутри недоступно и приглушено |
| `ui::next(h)`, `next(w, h)`, `avail()`, `spacer(h)`, `gap(g)` | слот вручную, остаток, отступ, промежуток |
| `ui::at(rect)` | следующий виджет — ровно в `rect` (панель инструментов на карте, ячейки) |
| `ui::prop("Налог", "percent")` | строка свойства: подпись слева (42 %), следующий виджет — справа |
| `ui::scrollToItem()` | прокрутить ближайшую область к последнему элементу (фокус с клавиатуры делает это сам); элемент уже виден — ничего, без перерисовки. Вызывать при смене выделения, а не каждый кадр: иначе колесо не листает список |

## 5. Виджеты

Все функции возвращают главное событие (`true` — нажато/изменено). Подробности о последнем элементе — `ui::lastItem()`: `id, rect, hovered, active, focused, clicked, rightClicked, doubleClicked, changed, deactivated`.

### Текст

```cpp
ui::label("Население", {.font = ui::Font::Small, .ink = ui::Ink::Muted, .icon = "population"});
ui::label(name, {.font = ui::Font::Display});                         // одна строка, многоточие
ui::label(pct(tax), {.tooltip = "Налог государства"});                 // с подсказкой — интерактивна; ID = текст + подсказка
ui::text("Абзац переносится по словам.", ui::Font::Body, ui::Ink::Dim);
ui::caption("Расы");                                                  // мелкий заголовок группы прописными
if (ui::link("Открыть хронику", "chronicle")) ...
ui::kbd("Ctrl+Shift+S");   ui::kbd({Key::S, ui::ModPrimary});           // клавиши-колпачки
ui::separator();   ui::separatorV();                                  // линия; в HStack — вертикальная
ui::icon("warning", ui::Ink::Warning, 18, "Есть ошибки");             // значок (с подсказкой)
ui::image(img, 64, 40, 6);                                            // изображение живёт до endFrame
```

### Кнопки

```cpp
if (ui::button("Применить", {.variant = ui::Variant::Primary, .icon = "check"})) ...
ui::button("Удалить", {.variant = ui::Variant::Danger, .icon = "trash"});
ui::button("Ещё", {.size = ui::Size::Small, .iconRight = "chevron-down"});
ui::button("Сохранить", {.fill = true, .shortcut = {Key::S, ui::ModPrimary}});   // срабатывает и по сочетанию
if (ui::iconButton("tool-knife", "Нож", {.toggled = tool == Knife, .shortcut = {Key::K, 0}})) ...
ui::iconButton("bell", "Уведомления", {.badge = true});
ui::iconToggle("eye", "Показать подписи", showLabels);
```

Варианты: `Primary` (золото, одна на панель/диалог), `Secondary` (поверхность с рамкой), `Ghost` (прозрачная), `Danger` (необратимое — и только с подтверждением), `Subtle` (приглушённый текст). Подсказка `iconButton` обязательна; сочетание добавляется в неё само.

### Переключатели и выбор

```cpp
ui::toggle("Правка границ", editBorders);                  // подпись слева, переключатель справа
ui::checkbox("Морская", sea);
ui::Check all = ui::Check::Mixed;  ui::checkbox("Все", all);   // неопределённое → включено
ui::radio("Малая", size, 0);  ui::radio("Средняя", size, 1);
ui::segmented("view", view, {{"map", "Карта"}, {"table", "Таблица"}});
ui::segmented("mode", mode, {{"mode-political", {}, "Политическая"}, {"mode-guilds", {}, "Гильдии"}}, {.fill = false}); // значки + подсказки
ui::slider("opacity", opacity, 0, 100, {.step = 5, .unit = "%"});        // пузырь со значением при перетаскивании
```

### Числа

```cpp
ui::numberField("tax", tax, {.min = 0, .max = 60, .unit = "%", .icon = "percent"});
ui::numberField("turns", turns, {.min = 1, .max = 99, .unit = "ход|хода|ходов", .steppers = true}); // склонение
ui::numberField("gold", gold, {.digits = 1, .label = "Казна"});
ui::numberField("reb", rebellion, {.sign = true, .readOnly = true});
```

Тянуть по полю или подписи влево-вправо — шаг за 4 точки (Shift — ×0,1, Ctrl — ×10). Щелчок без перетаскивания — ввод (всё выделено). Enter или уход фокуса — проверка: число ограничивается `min/max` и округляется до `digits`; неверный ввод — поле встряхивается, значение прежнее. Если текст не правили (щелчок и Tab), значение остаётся точным — показ, округлённый до `digits`, не записывается, изменения нет. Esc — отмена. Стрелки ↑↓ (Shift ×10) и колесо — шаг, если поле в фокусе. Кнопки −/+ повторяют шаг при удержании. Целые типы — шаблон `numberField(id, int&)`.

### Текстовые поля

```cpp
ui::textField("name", province.name, {.placeholder = "Название", .icon = "edit", .clearButton = true, .maxLength = 40});
ui::searchField("q", query);                                  // значок, крестик, живое изменение
ui::textArea("notes", notes, 120, {.placeholder = "Заметки"});
```

Каретка, выделение мышью (перетаскивание, двойной щелчок — слово, тройной — всё), Shift+стрелки, Ctrl+стрелки — по словам, Ctrl+Backspace/Delete — слово, Home/End (в многострочном — строка; с Ctrl — весь текст), Ctrl+A/C/X/V, отмена Ctrl+Z / повтор Ctrl+Y и Ctrl+Shift+Z (набор сливается в один шаг), ввод кириллицы через события `Text`, IME (`textInputRect`). `filter` — разрешённые символы, `readOnly` — только выделение и копирование.

### Выпадающие списки и цвет

```cpp
std::vector<ui::Option> owners;   // label, icon, color (точка), hint (справа), disabled
ui::combo("owner", ownerIdx, owners, {.noneLabel = "Нет владельца"});            // −1 — «нет»
ui::combo("terrain", t, {{"Равнина", "land"}, {"Горы", "mountain"}});
ui::combo("prov", provIdx, int(ids.size()), [&](int i) { return ui::Option{name(ids[i])}; });  // тысячи пунктов
ui::multiSelect("res", resources, options);                   // фишки с крестиком и «+»
ui::colorButton("color", faction.color);                      // образец + hex; всплывающий выбор
ui::colorPicker("inline", color, {.alpha = true});            // встроенный: палитра, HSV, оттенок, hex
```

Список открывается щелчком, Enter, Пробелом, ↓ или набором текста (поиск при > 8 пунктах, `search = 1` — всегда). ↑↓, PageUp/PageDown, Enter — выбор, Esc — закрыть. Длинные списки виртуализированы: `get(i)` вызывается только для видимых строк (и при поиске — для всех один раз на изменение запроса).

### Списки и ожидание

```cpp
for (Id pid : provinces) {
  ui::IdScope s{i64(pid)};
  if (ui::listItem(name(pid), {.dot = ownerColor(pid), .subtitle = region(pid), .hint = fmtShort(pop(pid)), .selected = sel == pid})) app.select(SelType::Province, pid);
  ui::dragSource("province", pid, name(pid), "province");          // строку можно перетащить (сразу после строки)
  if (ui::beginContextMenu("ctx")) { ...; ui::endMenu(); }      // правый щелчок по строке
}
ui::spinner(16);                                                   // идёт расчёт или загрузка
```

Для сотен строк — внутри `ui::VirtualList` (строка 32 точки, с подзаголовком — 44).

### Вкладки

```cpp
ui::tabs("ptabs", tab, {{"info", {}, "Обзор"}, {"coins", {}, "Экономика", 3}, {"guild", {}, "Гильдии", -1, ui::Tone::Danger}}, {.fill = true});
ui::tabs("t2", t2, {{"info", "Обзор"}, {"army", "Войска"}}, {.style = ui::TabStyle::Pill});
```

`Tab{icon, label, tooltip, badge, badgeTone}`: без подписи — только значок, подсказка обязательна; `badge > 0` — число, `−1` — точка. ←→ при фокусе.

### Показатели

```cpp
ui::stat("128 400", "Население", {.icon = "population", .delta = 2.1, .deltaText = "+2,1 %"});
ui::stat("18 %", "Восстание", {.icon = "rebellion", .tone = ui::Tone::Danger, .delta = 3, .invertDelta = true});
ui::progress(share, {.color = race.color, .label = true});
ui::meter(relation);                                          // −100…100, отметка нуля, цвет по знаку
ui::pie(slices, {.size = 108, .thickness = 15, .centerValue = "38 %", .centerLabel = "лидер"});   // кольцо; наведение — доля
ui::sparkline(history, {.height = 32});
ui::badge("12");  ui::badge("Новое", ui::Tone::Success);  ui::tag("Война", ui::Tone::Danger, "war");
if (ui::chip(name, {.color = faction.color, .clickable = true}) == ui::ChipAction::Click) select(id);
ui::avatar("Эдрик Третий", {.size = 36, .ring = true});      // изображение или инициалы
ui::flag(faction.flag, 66, 44);                               // gfx::drawFlag; флаг живёт до endFrame
if (ui::emptyState("army", "Войск пока нет.", "Новое войско", "plus")) createArmy();   // кнопка — в своей области ID («##emptyState»): подпись может совпадать с кнопкой над списком
```

## 6. Таблицы и деревья

```cpp
ui::Column cols[] = {{"Раса", nullptr, ui::fr(1.3f)},
                     {"Жители", "population", ui::fr(1), ui::Align::Right, true},   // sortable
                     {"Доля", nullptr, ui::fr(1.3f)}};
ui::Table t("races", cols, int(races.size()), {.selected = &selRace});   // .height > 0 — своя прокрутка
t.sort([&](int a, int b, int col) { return cmp(races[a], races[b], col); });   // по шапке: ↑, ↓, без сортировки
for (int i : t) {                         // только видимые строки; i — индекс данных
  t.text(races[i].name);                  // текст по выравниванию столбца
  t.text(fmtNum(races[i].pop));
  t.cell(); ui::progress(races[i].share, {.label = true});   // любая ячейка — любой виджет
}
if (t.footer()) { t.text("Итого"); t.text(fmtNum(total)); }
if (t.doubleClicked() >= 0) app.select(SelType::Character, ids[t.doubleClicked()]);
```

- Ширины `px/fr` с минимумом; шапка закрепляется у края видимой области родителя (таблица по содержимому) или стоит над своей прокруткой (`height > 0`); строки чередуются, наведение и выделение (золотая полоска слева); ↑↓ при фокусе меняют выделение; правый щелчок выделяет строку — меню строки: `if (int r = t.rightClicked(); r >= 0) { menuRow = r; ui::openContextMenu("row"); }` после цикла и `if (ui::beginMenu("row")) {...}` рядом.
- Строки, не попавшие в видимую часть, не строятся: 10 000 строк стоят столько же, сколько 20.
- Правка в ячейке — обычные виджеты (`numberField`, `combo`, `textField`): ID уже уникальны по строке.
- Добавление/удаление строк — кнопки в подвале или над таблицей; данные меняет вызывающий.
- Пустая таблица показывает `emptyIcon` и `emptyText`.

```cpp
ui::TreeNode n("Королевство Арден", {.icon = "crown", .selected = sel == id, .badge = "12"});
if (n.clicked()) sel = id;
if (n) { ui::TreeNode c("Эльвенмор", {.dot = color, .leaf = true}); }
```

## 7. Всплывающие слои

Слои рисуются в порядке: основной (панели в порядке объявления) → всплывающие → модальные (каждое следующее выше; их всплывающие — над ними) → уведомления → подсказка → перетаскиваемый объект. Порядок вызовов в коде на это не влияет.

```cpp
ui::iconButton("save", "Сохранить", {.shortcut = {Key::S, ui::ModPrimary}});
ui::tooltip("Своя подсказка", {Key::F2, 0});          // к последнему элементу; 350 мс, рядом — сразу
if (ui::beginTooltip(280)) { ui::label("Богатая"); ui::progress(0.4); ui::endTooltip(); }

if (ui::button("Ещё", {.iconRight = "chevron-down"})) ui::openPopup("more");   // якорь — последний элемент
if (ui::beginMenu("more")) {
  ui::menuHeader("Провинция");
  if (ui::menuItem("Переименовать", {.icon = "edit", .shortcut = {Key::F2, 0}})) rename();
  if (ui::beginSubmenu("Владелец", "crown")) { for (...) ui::menuItem(name, {.checked = mine}); ui::endSubmenu(); }
  ui::menuSeparator();
  if (ui::menuItem("Удалить", {.icon = "trash", .danger = true})) askDelete();
  ui::endMenu();
}
ui::label(province.name);
if (ui::beginContextMenu("ctx")) { ...; ui::endMenu(); }   // правый щелчок по последнему элементу

if (ui::beginPopup("filter", {.side = ui::Side::Below, .width = 260})) { ...; ui::endPopup(); }   // свой поповер
```

- Всплывающее окно переворачивается, если не помещается, и прижимается к краю экрана. Размер — по содержимому (первый кадр — невидимый замер); если в нём есть элементы «на всю ширину», задайте `width`.
- Щелчок вне закрывает (повторный щелчок по открывателю — закрывает, а не открывает заново); Esc закрывает верхнее; ↑↓ Enter в меню, → открывает подменю, ← закрывает.

```cpp
if (ui::beginModal("delete", {.title = "Удалить провинцию?", .icon = "trash", .tone = ui::Tone::Danger}, &askOpen)) {
  ui::text("Земли отойдут соседям. Действие можно отменить Ctrl+Z.", ui::Font::Body, ui::Ink::Dim);
  {
    ui::ModalFooter f;
    if (ui::button("Отмена")) ui::closeModal();
    if (ui::button("Удалить", {.variant = ui::Variant::Danger, .isDefault = true})) { app.act(...); ui::closeModal(); }
  }
  ui::endModal();
}
```

Модальное окно: затемнение и размытие всего, что ниже (фон кешируется, пока нижние слои не меняются), заголовок со значком, крестик, ловушка фокуса (Tab ходит только внутри), Esc — закрыть (если не поглотило поле или всплывающее), Enter — кнопка `isDefault` (поле при Enter сначала фиксирует значение), всё под окном недоступно. Закрытие — `*open = false`, `ui::closeModal()` изнутри или Esc; исчезает плавно.

```cpp
ui::toast("Мир сохранён", ui::Tone::Success);                     // справа внизу, 4 с, наведение — пауза
ui::toast("Не удалось открыть архив", ui::Tone::Danger, "error", 8);
```

## 8. Перетаскивание

```cpp
ui::chip(unitName, {.icon = "army", .clickable = true});
ui::dragSource("unit", unitId, unitName, "army");                 // источник — последний элемент
ui::Card c({.title = "Гарнизон"}); ...
ui::button("Сюда");
if (auto id = ui::dropTarget("unit")) app.act("Перевести отряд", [&](Tx& tx) { ... *id ... });
```

Перенос начинается после сдвига на 3 точки; за указателем — фишка со значком и подписью; цель подсвечивается золотой рамкой.

## 9. Свободное рисование и свои виджеты

```cpp
RectF r = ui::next(0, 240);
ui::custom(r, [&](gfx::Canvas& c, RectF dev, float scale) { techTree.draw(c, dev, scale); });  // физические пиксели, отсечение dev
ui::Interaction it = ui::interact(ui::id("tree"), r, ui::IfFocusable);   // наведение, нажатие, перетаскивание, фокус
if (it.dragging) pan += {it.dx, it.dy};
ui::draw::rect(r, ui::theme().surface3, 8);  ui::draw::icon("star", r, ui::toneColor(ui::Tone::Accent));
```

- `ui::custom` — карта, дерево технологий, графики: колбэк вызывается при сведении слоя, данные, на которые он ссылается, должны жить до `endFrame`.
- `ui::draw::*` — примитивы в точках интерфейса (прямоугольник, обводка, градиент, тень, круг, кольцо, линия, контур, значок, текст, изображение, отсечение).
- `interact(id, rect, flags)`: `IfFocusable` (Tab, Enter/Пробел — щелчок), `IfAllowOverlap` (фон под другими элементами — строка, холст под кнопками), `IfRightButton`, `IfMiddleButton`. Результат: `hovered, pressed, held, released, clicked, rightClicked, middleClicked, doubleClicked, dragging, dx/dy, focused, keyActivated`.
- Состояние: `auto& s = ui::state<MyState>(ui::id("tree"));` — живёт, пока запрашивается (уборка через ~5 с и 300 кадров без обращений). Анимации: `float t = ui::animate(id, hovered ? 1 : 0, 0.12f);`, `ui::animateColor`. Своя анимация — `ui::requestRedraw()`.
- Фокус с клавиатуры: `ui::setKeyboardFocus(ui::id("search"))` до вызова поля (например, по Ctrl+F).

## 10. Соглашения для авторов панелей

1. **Фиксация значений.** `textField`/`textArea` по умолчанию меняют значение только по Enter или уходу фокуса — одна запись в истории. `numberField`, `slider`, `colorButton` при перетаскивании меняют значение каждый кадр: `if (ui::numberField("tax", v)) app.act("Налог", fn, {.coalesce = "tax:" + std::to_string(pid)});` — подряд идущие изменения сливаются в один шаг отмены (`ui::lastItem().active` — ещё тянут). Поле теряет правку, если его перестали рисовать до фиксации.
2. **ID.** Уникальные подписи в области; списки — через таблицу/виртуальный список или `ui::IdScope s{id};`. Не используйте индексы, которые меняются при сортировке, — берите ID сущности.
3. **Раскладка.** Панель → разделы (`Section`) → строки свойств (`prop` + виджет) и таблицы. Вкладки инспектора — `tabs` с `fill = true` и значками; длинные панели — внутри `Scroll`. Выравнивание по сетке 4/8; ничего не позиционировать «на глаз» числами, кроме `at` для плавающих инструментов.
4. **Минимум текста.** Значок вместо слова, подсказка на каждом значке, числа — крупно (`stat`, `Font::Number`), подписи — мелко (`Small`, `Caption`), пустое состояние — одна строка и кнопка. Никаких технических сообщений.
5. **Цвет — из темы.** Золото (`Primary`, `Tone::Accent`) — для главного действия и выделения, не больше одной основной кнопки на виду. Опасное — `Danger` и подтверждение модальным окном.
6. **Числа** — `fmtNum/fmtPct/fmtSigned/fmtShort/nTurns`, единицы со склонением — `"ход|хода|ходов"`.
7. **Ничего не блокирует кадр**: долгие расчёты — в `jobs`, интерфейс показывает `progress` и уведомление.
8. **Проверка панели** — тест без окна: `ui::beginFrame/endFrame` на `gfx::Image`, события через `ui::onEvent` (см. `src/tests/test_ui_util.h`: `click`, `type`, `key`, `dragTo`, `wheel`, `settle`, `save`).

## 11. Проверки и цифры

- `./build.sh test-ui --test` — ядро (ID, наведение, фокус, раскладка, состояние), текст (каретка, выделение, буфер обмена, отмена, кириллица), виджеты (числа, списки, таблицы, прокрутка, меню, модальные окна, уведомления, перетаскивание), галерея PNG в .wmma/regnum-tests (файлы ui_*) (тёмная и светлая темы, 125 % и 150 %, композиции «Провинция» и «Государство»).
- Кадр 1600 × 1000 с двумя панелями, таблицами, диаграммами и стеклянной легендой — около 5 мс (построение — 0,1 мс), лист всех элементов — около 1,6 мс (Release, один поток). В покое кадры не рисуются.

## 12. Ограничения

- Одна графема = одна кодовая точка (как в `gfx/text`): составные эмодзи и комбинируемые знаки редактируются по точкам.
- Раскрытие разделов, сортировка таблиц и замеры рядов помнятся за время работы приложения (не сохраняются в проект).
- Размер всплывающего окна по содержимому определяется за один невидимый кадр; элементы «на всю ширину» требуют явного `width`.
- Колбэки `custom`, изображения `image/avatar` и флаги `flag` берутся по ссылке до `endFrame`.
