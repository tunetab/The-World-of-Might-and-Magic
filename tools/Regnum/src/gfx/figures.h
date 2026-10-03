// Regnum — фигурки и метки карты: войско, флот, столица, штаб гильдии, битва.
//
// Фигурка войска и флота — жетон: круглый тёмный постамент с кольцом цвета фракции и светлым силуэтом
// (воин со щитом цвета фракции и копьём с флажком; галеон с парусами цвета фракции над волной).
// center — точка карты (центр постамента), size — диаметр постамента в единицах холста; наконечник
// копья и флажок мачты немного выступают над кольцом, тень — под ним (см. figureBounds).
// Готовые изображения кешируются по (вид, размер в пикселях устройства, цвета, флаги, доля пикселя).
#pragma once
#include "gfx/canvas.h"

namespace rg::gfx {

void drawArmyFigure(Canvas& c, Pt center, float size, Color faction, bool selected = false, bool allied = false,
                    Color ally2 = Color(0, 0, 0, 0));
void drawFleetFigure(Canvas& c, Pt center, float size, Color faction, bool selected = false, bool allied = false,
                     Color ally2 = Color(0, 0, 0, 0));
// Столица: круглый знак цвета фракции со светлой звездой под короной. size — диаметр знака.
void drawCapitalMarker(Canvas& c, Pt center, float size, Color faction);
// Штаб гильдии: ромб цвета гильдии со светлым зданием с колоннами. size — сторона описанного квадрата.
void drawHqMarker(Canvas& c, Pt center, float size, Color guild);
// Битва: скрещённые мечи на огненной звезде. size — диаметр звезды.
void drawBattleMarker(Canvas& c, Pt center, float size);

// Габариты фигурки (с наконечником, флажком, ореолом выбора и тенью) — для попадания мышью и перерисовки.
RectF figureBounds(Pt center, float size);
// Габариты знаков столицы, штаба и битвы.
RectF markerBounds(Pt center, float size);

// Кеш готовых фигурок (потокобезопасно).
void clearFigureCache();
size_t figureCacheSize();

}  // namespace rg::gfx
