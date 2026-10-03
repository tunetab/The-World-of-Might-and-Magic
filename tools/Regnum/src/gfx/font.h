// Regnum — шрифты TrueType/OpenType (контуры glyf): разбор, метрики, кернинг, контуры и растеризация глифов.
//
// Файл шрифта — недоверенные данные: каждое чтение проверяет границы, некорректный шрифт не роняет программу.
// Поддержано: TTF и TTC; таблицы head, hhea, hmtx, maxp, loca, glyf (простые и составные глифы с преобразованиями),
// cmap (форматы 0, 4, 6, 12), kern (формат 0, версии Microsoft и Apple), GPOS (парный кернинг 'kern', форматы 1 и 2),
// OS/2, name, post. Вариативные шрифты — экземпляр по умолчанию (gvar не применяется). Шрифты только с CFF не загружаются.
#pragma once
#include <memory>

#include "gfx/image.h"
#include "gfx/path.h"

namespace rg::gfx {

// Контур глифа из квадратичных кривых. Каждый контур начинается точкой на кривой; две контрольные точки подряд
// не встречаются (подразумеваемые точки TrueType уже вставлены). Контуры замкнуты неявно.
struct GlyphOutline {
  std::vector<Pt> pts;
  std::vector<u8> onCurve;          // 1 — точка на кривой, 0 — контрольная точка квадратичной кривой
  std::vector<u32> contourEnds;     // индекс последней точки каждого контура (включительно)

  void clear() { pts.clear(); onCurve.clear(); contourEnds.clear(); }
  bool empty() const { return contourEnds.empty(); }
  RectF bounds() const;             // по всем точкам (включая контрольные — выпуклая оболочка кривых)
  void transform(const Affine& m);
  // Утолщение контура на strength (в текущих единицах) с каждой стороны; дырки сужаются.
  void embolden(float strength) { embolden(strength, strength); }
  void embolden(float sx, float sy);   // раздельно по осям
  void appendTo(Path& out, const Affine& m = {}) const;
};

class Font {
 public:
  // Число начертаний в файле: 1 для TTF/OTF, n для TTC; 0 — не шрифт.
  static int faceCount(std::string_view data);
  // Загрузка начертания. data должна жить вместе со шрифтом (хранится shared_ptr). nullptr — ошибка (текст в error).
  static std::shared_ptr<const Font> load(std::shared_ptr<const std::string> data, int faceIndex = 0, std::string* error = nullptr);
  static std::shared_ptr<const Font> loadFile(const std::string& utf8Path, int faceIndex = 0, std::string* error = nullptr);

  ~Font();
  Font(const Font&) = delete;
  Font& operator=(const Font&) = delete;

  // Имена (предпочтительно типографские nameID 16/17, иначе 1/2).
  const std::string& family() const { return family_; }
  const std::string& subfamily() const { return subfamily_; }
  const std::string& fullName() const { return fullName_; }
  int weightClass() const { return weightClass_; }       // 100..900 (OS/2), 400 по умолчанию
  int widthClass() const { return widthClass_; }         // 1..9 (OS/2), 5 — обычная ширина
  bool italic() const { return italic_; }
  bool monospace() const { return monospace_; }
  bool variable() const { return variable_; }             // есть fvar (используется экземпляр по умолчанию)

  // Метрики в единицах шрифта; descender отрицателен.
  int unitsPerEm() const { return upem_; }
  int ascender() const { return ascender_; }
  int descender() const { return descender_; }
  int lineGap() const { return lineGap_; }
  int capHeight() const { return capHeight_; }
  int xHeight() const { return xHeight_; }
  int glyphCount() const { return numGlyphs_; }

  u16 glyphIndex(u32 codepoint) const;                    // 0 — нет глифа
  bool hasGlyph(u32 codepoint) const { return glyphIndex(codepoint) != 0; }
  int advance(u16 glyph) const;                           // ширина продвижения (единицы шрифта)
  int leftSideBearing(u16 glyph) const;
  int kerning(u16 left, u16 right) const;                 // поправка продвижения левого глифа пары
  bool hasKerning() const;
  // Контур в единицах шрифта (y вверх). false — глиф пуст или повреждён (out очищен).
  bool outline(u16 glyph, GlyphOutline& out) const;
  std::string_view data() const;
  u32 uid() const;                                        // уникальный номер загруженного начертания (ключ кешей)

  struct Impl;

 private:
  Font();
  std::unique_ptr<Impl> d_;
  std::string family_, subfamily_, fullName_;
  int weightClass_ = 400, widthClass_ = 5;
  bool italic_ = false, monospace_ = false, variable_ = false;
  int upem_ = 1000, ascender_ = 800, descender_ = -200, lineGap_ = 0, capHeight_ = 700, xHeight_ = 500, numGlyphs_ = 0;
};

// ---------------------------------------------------------------- растеризация

// Растр глифа: покрытие 0..255 и смещение левого верхнего угла от точки пера (целые пиксели, y вниз).
struct GlyphBitmap {
  Mask mask;
  int left = 0, top = 0;
};

// Тон текста — выбирает кривую усиления покрытия (тёмный текст на светлом фоне требует меньшего усиления).
enum class TextTone : u8 { Dark = 0, Mid = 1, Light = 2 };
TextTone toneOf(Color textColor);

// Синтетическая жирность (когда в системе нет файла нужного начертания).
enum class Synth : u8 { None = 0, Semibold = 1, Bold = 2 };
// Утолщение контура (пикселей с каждой стороны) для синтетического начертания при данном кегле.
float synthStrength(Synth s, float sizePx);
// Добавка к ширине продвижения при синтетическом утолщении (пиксели).
float synthAdvance(Synth s, float sizePx);
// То же для конкретного шрифта: у моноширинного сетка не меняется (утолщение симметрично, без добавки).
float synthAdvance(const Font& f, Synth s, float sizePx);

// Точная растеризация: площадь покрытия каждого пикселя (правило ненулевой обмотки для непересекающихся контуров,
// перекрытия насыщаются). Контур — в пикселях устройства, y вниз. Возвращает пустую маску для пустого контура.
GlyphBitmap rasterizeOutline(const GlyphOutline& o, TextTone tone = TextTone::Mid, bool applyTone = false);

// Растр глифа: кегль sizePx (квантуется до ¼ px), субпиксельный сдвиг subX ∈ 0..3 (четверти пикселя).
GlyphBitmap renderGlyph(const Font& f, u16 glyph, float sizePx, int subX, Synth synth, TextTone tone);

// Потокобезопасный кеш растров глифов с вытеснением LRU по памяти.
struct GlyphRequest {
  const Font* font = nullptr;
  u16 glyph = 0;
  u16 size4 = 0;     // кегль × 4
  u8 subX = 0;       // 0..3
  Synth synth = Synth::None;
  TextTone tone = TextTone::Mid;
};
class GlyphCache {
 public:
  explicit GlyphCache(size_t capacityBytes = 32u << 20);
  ~GlyphCache();
  std::shared_ptr<const GlyphBitmap> get(const GlyphRequest& r);
  // Пакетный запрос под одной блокировкой (промахи растеризуются вне блокировки).
  void getMany(const GlyphRequest* req, size_t n, std::shared_ptr<const GlyphBitmap>* out);
  void setCapacity(size_t bytes);
  void clear();
  struct Stats { size_t entries, bytes, capacity; u64 hits, misses; };
  Stats stats() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> d_;
};
GlyphCache& glyphCache();   // общий кеш процесса (32 МБ)

// ---------------------------------------------------------------- системные шрифты (fontsys.cpp)
enum class FontFamily : u8 { UI = 0, Display = 1, Mono = 2 };      // без засечек / с засечками / моноширинный
enum class FontWeight : u8 { Regular = 0, Semibold = 1, Bold = 2 };

struct FontFace {
  std::string path;                       // UTF-8
  int index = 0;                          // номер начертания в TTC
  std::shared_ptr<const Font> font;       // для запасных шрифтов может быть пуст (загрузка по требованию)
  Synth synth = Synth::None;              // синтетическое утолщение (нет файла нужного начертания)
};
struct SystemFonts {
  FontFace faces[3][3];                   // [FontFamily][FontWeight]
  std::vector<FontFace> fallbacks;        // запасные шрифты по порядку (символы, прочие письменности)
  std::vector<std::string> dirs;          // просмотренные каталоги
};
// Поиск системных шрифтов с кириллицей: Windows %WINDIR%/Fonts и шрифты пользователя, macOS /System/Library/Fonts,
// /Library/Fonts, ~/Library/Fonts, Linux /usr/share/fonts, /usr/local/share/fonts, ~/.local/share/fonts, ~/.fonts
// и $XDG_DATA_DIRS. Дополнительные каталоги — переменная окружения REGNUM_FONT_DIRS (через «;»), они просматриваются первыми.
// false — нет ни одного пригодного шрифта (сообщение по-русски в error).
bool findSystemFonts(SystemFonts& out, std::string* error = nullptr);
// То же только в указанных каталогах (UTF-8; обход рекурсивный) — для переносимых сборок и проверок.
bool findFontsIn(const std::vector<std::string>& dirs, SystemFonts& out, std::string* error = nullptr);
// Каталоги системных шрифтов, существующие или нет, в порядке приоритета.
std::vector<std::string> systemFontDirs();
// Покрывает ли шрифт базовую кириллицу и латиницу.
bool hasCyrillic(const Font& f);

}  // namespace rg::gfx
