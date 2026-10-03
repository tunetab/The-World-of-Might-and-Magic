// Regnum — обводка контуров: смещение ломаных, соединения miter/round/bevel, концы butt/round/square, пунктир.
//
// Результат — контур, который заливается правилом NonZero. Все части обводки ориентированы одинаково,
// поэтому самопересечения и перекрытия на изломах дают покрытие 1, а не двойное смешивание.
#pragma once
#include "gfx/canvas.h"

namespace rg::gfx {

// Контур обводки src в тех же координатах (out очищается). tol — допуск аппроксимации кривых и скруглений.
// cull — необязательная область видимости: кривые целиком вне её (с запасом на толщину) спрямляются.
void strokeToPath(const Path& src, const Stroke& s, float tol, Path& out, const RectF* cull = nullptr);
Path strokeToPath(const Path& src, const Stroke& s, float tol = 0.1f);

// Обводка одной ломаной (добавляется к out).
// cull — необязательная область видимости (как в strokeToPath): невидимые отрезки пунктира не строятся.
void strokePolyline(const Pt* pts, size_t n, bool closed, const Stroke& s, float tol, Path& out,
                    const RectF* cull = nullptr);

// Разбиение ломаной на штрихи пунктира; каждый штрих — открытая ломаная (не менее двух точек).
// Шаблон нечётной длины повторяется дважды (как в SVG). false — шаблон недопустим (пуст, отрицательные
// или нечисловые значения, нулевая сумма) либо штрихов слишком много: линия рисуется сплошной.
// cull — область видимости (уже расширенная на толщину): штрихи на отрезках целиком вне неё не выдаются,
// фаза шаблона при этом сохраняется; лимит штрихов считается по видимой длине.
bool dashPolyline(const Pt* pts, size_t n, bool closed, const std::vector<float>& pattern, float offset,
                  const std::function<void(const Pt*, size_t)>& emit, const RectF* cull = nullptr);

double polylineLength(const Pt* pts, size_t n, bool closed);

}  // namespace rg::gfx
