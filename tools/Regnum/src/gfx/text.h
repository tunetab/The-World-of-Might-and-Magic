// Regnum — текст: системные шрифты, метрики, измерение, раскладка, отрисовка и поддержка редактирования.
//
// Раскладка: кернинг (GPOS/kern), перенос по пробелам, дефисам, тире и мягким переносам, разрыв длинных слов,
// табуляция (шаг — 4 пробела), принудительные переводы строк (\n, U+2028, U+2029), ограничение числа строк
// и многоточие «…». Невидимые символы (U+200B, U+200D, U+FEFF и т. п.) не отображаются; отсутствующие в шрифтах
// пробелы U+00A0, U+2000–U+200A, U+202F рисуются пробелами нужной ширины. Символ без глифа ищется в цепочке
// запасных шрифтов семейства. Одна кодовая точка — одна позиция каретки (графема ≈ кодовая точка).
//
// Координаты — логические пиксели холста. Растр глифов учитывает масштаб текущего преобразования холста
// (кегль растеризации = кегль × масштаб), поворот и неравномерный масштаб рисуются контурами.
// Все функции потокобезопасны; шрифты ищутся при первом обращении (или явным initFonts).
#pragma once
#include "gfx/canvas.h"
#include "gfx/font.h"

namespace rg::gfx {

enum class Align : u8 { Left, Center, Right };
enum class VAlign : u8 { Top, Middle, Bottom };

struct TextStyle {
  FontFamily family = FontFamily::UI;
  FontWeight weight = FontWeight::Regular;
  float size = 13;            // кегль, px (квантуется до ¼ px)
  float letterSpacing = 0;    // добавка после каждого символа, px
  float lineHeight = 0;       // множитель кегля; 0 — межстрочное расстояние шрифта
  bool operator==(const TextStyle&) const = default;
};

// Поиск системных шрифтов (повторные вызовы возвращают первый результат). false — нет шрифта с кириллицей,
// в error — сообщение для пользователя.
bool initFonts(std::string* error = nullptr);
bool fontsReady();
// Описание выбранных шрифтов (для журнала и окна «О программе»): «UI Regular: Segoe UI — C:/Windows/Fonts/segoeui.ttf».
std::vector<std::string> fontReport();
// Основной шрифт стиля и его синтетическое утолщение (nullptr — шрифтов нет).
const Font* primaryFont(FontFamily family, FontWeight weight, Synth* synth = nullptr);

struct FontMetrics {
  float ascent = 0;       // над базовой линией, px
  float descent = 0;      // под базовой линией (положительное), px
  float lineGap = 0;
  float lineHeight = 0;   // шаг строк с учётом TextStyle::lineHeight
  float capHeight = 0;
  float xHeight = 0;
  float baseline = 0;     // от верха строки до базовой линии
};
FontMetrics metrics(const TextStyle& style);

// Ширина текста без переноса (для нескольких строк — самая широкая); хвостовые пробелы строки не учитываются.
float measureText(std::string_view utf8, const TextStyle& style);

enum class GlyphKind : u8 {
  Normal,      // видимый глиф
  Space,       // пробел с возможностью переноса
  NoBreak,     // неразрывный пробел
  Tab,
  Invisible,   // нулевой ширины (управляющие, U+200B, мягкий перенос без разрыва и т. п.)
  Hyphen,      // дефис, добавленный при переносе по U+00AD
  Ellipsis,    // многоточие обрезанного текста
};

struct TextGlyph {
  float x = 0;              // левый край позиции (координаты раскладки)
  float advance = 0;
  u32 byte = 0;             // начало кодовой точки в тексте
  u16 glyph = 0;            // номер глифа в шрифте face цепочки
  u8 face = 0;
  GlyphKind kind = GlyphKind::Normal;
};

struct TextLine {
  u32 begin = 0, end = 0;   // байты строки: end — перед переводом строки (хвостовые пробелы включены)
  u32 next = 0;             // начало следующей строки (end + длина перевода строки или end при мягком переносе)
  u32 glyphBegin = 0, glyphEnd = 0;
  float x = 0;              // сдвиг выравнивания
  float y = 0;              // верх строки
  float width = 0;          // ширина без хвостовых пробелов
  float baseline = 0;       // базовая линия от верха раскладки
  bool hardBreak = false;   // строка закончилась переводом строки
};

struct TextLayout {
  TextStyle style;
  float size = 0;           // квантованный кегль
  float lineHeight = 0, ascent = 0, descent = 0;
  float width = 0;          // самая широкая строка
  float height = 0;         // число строк × шаг
  float boxWidth = 0;       // ширина, по которой выравнивались строки
  bool truncated = false;   // часть текста не показана (maxLines или многоточие)
  u32 textBytes = 0;
  std::vector<TextGlyph> glyphs;
  std::vector<TextLine> lines;
};

struct LayoutOptions {
  float maxWidth = 0;       // ≤ 0 — без ограничения
  int maxLines = 0;         // 0 — без ограничения
  bool wrap = true;         // переносить по словам при maxWidth > 0
  bool ellipsis = false;    // многоточие при обрезке (строки по maxLines или ширине без переноса)
  Align align = Align::Left;
};

TextLayout layoutText(std::string_view utf8, const TextStyle& style, const LayoutOptions& opt);
// Краткая форма: maxWidth > 0 — перенос по словам; при maxLines = 1 и ellipsis — обрезка по символам «Длинное наз…».
TextLayout layoutText(std::string_view utf8, const TextStyle& style, float maxWidth = 0, int maxLines = 0, bool ellipsis = false);
// То же с повторным использованием памяти out (для горячих путей).
void layoutTextInto(TextLayout& out, std::string_view utf8, const TextStyle& style, const LayoutOptions& opt);

// Отрисовка: y — верх строки (не базовая линия).
void drawText(Canvas& c, std::string_view utf8, const TextStyle& style, float x, float y, const Paint& paint);
void drawLayout(Canvas& c, const TextLayout& layout, float x, float y, const Paint& paint);
// Текст в прямоугольнике: выравнивание, перенос, предел строк, многоточие. Возвращает занятый прямоугольник.
RectF drawTextBox(Canvas& c, std::string_view utf8, const TextStyle& style, RectF box, const Paint& paint, Align align = Align::Left,
                  VAlign valign = VAlign::Middle, bool wrap = false, int maxLines = 0, bool ellipsis = true);
// Контуры глифов раскладки (для ореолов подписей карты и произвольных преобразований).
void layoutPath(const TextLayout& layout, float x, float y, Path& out);

// Растры глифов раскладки в пикселях устройства: (x, y) — положение верха раскладки на устройстве,
// scale — пикселей устройства на логический пиксель. sink получает маску и её левый верхний угол.
using GlyphSink = std::function<void(const Mask& mask, int x, int y)>;
void rasterizeLayout(const TextLayout& layout, float x, float y, float scale, TextTone tone, const GlyphSink& sink);

// ---------------------------------------------------------------- редактирование
size_t hitTest(const TextLayout& layout, float x, float y);          // ближайшая позиция каретки (байт)
Pt caretPos(const TextLayout& layout, size_t byteIndex);             // верх каретки; высота — layout.lineHeight
size_t lineOf(const TextLayout& layout, size_t byteIndex);
std::vector<RectF> selectionRects(const TextLayout& layout, size_t a, size_t b);  // прямоугольники выделения [a, b)
// Текст, укороченный до ширины с многоточием (без изменений, если помещается).
std::string ellipsize(std::string_view utf8, const TextStyle& style, float maxWidth);

}  // namespace rg::gfx
