// Regnum — разбор данных пути SVG (атрибут d) в gfx::Path и обратная запись.
//
// Поддерживаются все команды SVG 1.1/2: M m L l H h V v C c S s Q q T t A a Z z, неявные повторы
// (после M — L), числа через пробел, запятую или знак («10-5», «.5.5»), экспоненты, флаги дуг без
// разделителей («a1 1 0 01 2 2»). При ошибке, как в браузерах, сохраняется всё, что разобрано до неё.
#pragma once
#include "gfx/path.h"

namespace rg::gfx {

struct SvgPathError {
  size_t pos = 0;      // байтовая позиция ошибки в строке
  std::string msg;     // описание (для журнала разработчика)
};

// Добавить контуры из строки d к out. false — строка содержит ошибку (out содержит разобранное до неё).
bool parseSvgPath(std::string_view d, Path& out, SvgPathError* err = nullptr);

// Удобная форма: ошибки игнорируются (возвращается разобранная часть).
Path svgPath(std::string_view d);

// Запись контура строкой SVG (абсолютные команды M, L, Q, C, Z). digits — знаков после запятой.
std::string toSvgPath(const Path& p, int digits = 3);

// Число в компактной записи для строк SVG: без лишних нулей, «-0» → «0», не более digits знаков дроби.
std::string svgNum(double v, int digits = 3);

}  // namespace rg::gfx
