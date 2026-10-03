// Regnum — значки интерфейса и общий механизм векторных глифов (значки, эмблемы, фигурки).
//
// Значок — набор слоёв на сетке 24 × 24: обводки (по умолчанию 1,75, круглые концы и соединения),
// заливки и «ластики» (вырезают зазоры, например перечёркивание глаза). Слои сводятся в одну маску
// покрытия (объединение по максимуму, ластик — вычитание), поэтому полупрозрачный цвет не даёт
// двойного наложения на пересечениях. Маски кешируются по (глиф, размер, толщина).
//
// Подгонка к пикселям (до 40 px): вертикальные и горизонтальные штрихи и края заливок выравниваются
// по сетке устройства, толщина слегка округляется — значки чёткие на 16, 18, 20 и 24 px.
#pragma once
#include "gfx/canvas.h"

namespace rg::gfx {

// ---------------------------------------------------------------- значки по имени
bool hasIcon(std::string_view name);
// Нарисовать значок по центру rect (квадрат со стороной min(w, h)). Неизвестное имя — заметный значок «?»
// и одна запись в журнал на имя.
void drawIcon(Canvas& c, std::string_view name, RectF rect, Color color, float strokeScale = 1);
// Все имена в порядке определения (общие, предметные, величины, постройки, отряды, корабли, инструменты),
// затем фигурки карты fig-army, fig-fleet, fig-allied-army, fig-allied-fleet (gfx/figures.h; цвет значка —
// цвет фракции, союзник — его осветлённый вариант). «ruler» — правитель (трон), «measure» — линейка-измеритель.
const std::vector<std::string>& iconNames();

// ---------------------------------------------------------------- векторные глифы
struct VecLayer {
  enum Kind : u8 {
    Stroke,        // обводка контура
    Fill,          // заливка NonZero
    FillEvenOdd,   // заливка EvenOdd (дыры)
    Erase,         // вычесть заливку
    EraseStroke,   // вычесть обводку (зазор вокруг линии)
  };
  Kind kind = Stroke;
  float width = 0;   // толщина обводки в единицах сетки; 0 — толщина по умолчанию
  Path path;         // в единицах сетки
};

struct VecGlyph {
  float grid = 24;                // сторона сетки
  std::vector<VecLayer> layers;
  u64 id = 0;                     // ненулевой — глиф из реестра, его маски кешируются
};

struct GlyphStyle {
  float strokeWidth = 1.75f;      // толщина по умолчанию (единицы сетки)
  float strokeScale = 1;          // множитель толщины всех обводок
  Join join = Join::Round;
  Cap cap = Cap::Round;
  bool hint = true;               // подгонка к пикселям на малых размерах
  float hintMaxPx = 40;           // подгонка выполняется, пока сторона глифа не больше этого (px устройства)
};

// Нарисовать глиф в квадрат по центру rect цветом color (с учётом преобразования, отсечения и прозрачности холста).
void drawGlyph(Canvas& c, const VecGlyph& g, RectF rect, Color color, const GlyphStyle& style = {});

// Маска покрытия глифа: toDevice — из единиц сетки в пиксели устройства, маска покрывает область
// [ox, ox + out.w) × [oy, oy + out.h). Подгонка применяется, только если toDevice — сдвиг и равномерный масштаб.
void renderGlyphMask(const VecGlyph& g, const Affine& toDevice, Mask& out, int ox, int oy, const GlyphStyle& style = {});

// Контур объединения всех слоёв глифа (обводки развёрнуты, ластики не учитываются) — для попадания мышью
// и экспорта. В единицах сетки.
Path glyphOutline(const VecGlyph& g, const GlyphStyle& style = {});

// Глиф значка из реестра (nullptr — нет такого имени).
const VecGlyph* iconGlyph(std::string_view name);

// Проблемы реестра (ошибки разбора путей, пустые слои, выход за сетку) — пусто, если всё в порядке.
std::vector<std::string> iconRegistryIssues();

// Сбросить кеш масок (значки и эмблемы). Потокобезопасно.
void clearGlyphCache();
size_t glyphCacheSize();

}  // namespace rg::gfx
