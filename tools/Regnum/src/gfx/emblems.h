// Regnum — геральдические эмблемы для флагов: залитые силуэты на сетке 100 × 100.
//
// Эмблема рисуется одним цветом по центру квадрата rect (сторона — min(w, h)); детали — вырезы в силуэте.
// Механизм тот же, что у значков (gfx/icons.h): маски кешируются, мелкие размеры подгоняются к пикселям.
#pragma once
#include "gfx/icons.h"

namespace rg::gfx {

// Имена эмблем в порядке каталога: crown, tower, castle, star, sun, moon, tree, lily, sword, swords, shield,
// skull, anchor, ship, wheat, gem, hammer, axe, bow, key, eye, flame, eagle, lion, dragon, wolf, bull, horse,
// serpent, kraken, rune, rose, griffin, bear.
const std::vector<std::string>& emblemNames();
bool hasEmblem(std::string_view name);
// Подпись для выбора эмблемы («Корона»); пустая строка — неизвестное имя.
std::string_view emblemTitle(std::string_view name);
// Нарисовать эмблему; пустое имя — ничего, неизвестное — ничего и одна запись в журнал на имя.
void drawEmblem(Canvas& c, std::string_view name, RectF rect, Color color);

const VecGlyph* emblemGlyph(std::string_view name);
// Проблемы реестра (ошибки путей, выход за сетку) — пусто, если всё в порядке.
std::vector<std::string> emblemRegistryIssues();

}  // namespace rg::gfx
