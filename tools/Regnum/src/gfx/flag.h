// Regnum — отрисовка флага (rg::Flag из core/world.h): узор из трёх цветов, эмблема, лёгкая тканевая
// светотень, скругление и рамка; флаг-изображение (PNG/JPEG в Flag::png) — с заполнением «cover».
//
// Цвета узоров (colors[0..2] = c0, c1, c2):
//   solid — c0;  h2 / v2 — c0, c1;  h3 / v3 — c0, c1, c2;
//   cross (скандинавский, сдвинут к древку) — поле c0, крест c1, узкий внутренний крест c2 (если c2 ≠ c1);
//   saltire — поле c0, косой крест c1, узкий внутренний c2 (если c2 ≠ c1);
//   quarters — c0 в 1-й и 4-й четвертях, c1 во 2-й и 3-й;  bend — над перевязью c0, перевязь c1, под ней c2;
//   chevron (стропило) — поле c0, стропило c1, под стропилом c2;  border — поле c0, кайма c1;
//   canton — поле c0, крыж c1;  chief — поле c0, глава c1;  pale — поле c0, столб c1.
// Эмблема — по центру поля; в крыже (canton), на перекрестье (cross), под главой (chief), под стропилом (chevron),
// в столбе (pale) — по месту узора. Края полос при отрисовке без поворота совпадают с границами пикселей.
#pragma once
#include "gfx/canvas.h"

namespace rg {
struct Flag;
}

namespace rg::gfx {

void drawFlag(Canvas& c, const Flag& flag, RectF rect, float cornerRadius, bool frame = true);

// Сбросить кеш декодированных изображений флагов (потокобезопасно).
void clearFlagCache();
size_t flagCacheSize();

}  // namespace rg::gfx
