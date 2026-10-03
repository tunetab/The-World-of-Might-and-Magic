// Regnum — арифметика premultiplied-пикселей и ядра смешивания (внутренний заголовок gfx).
// Пиксель: 0xAARRGGBB, каналы premultiplied. Два канала обрабатываются одним умножением (маска 0x00FF00FF).
#pragma once
#include "gfx/canvas.h"

namespace rg::gfx::px {

constexpr u32 kRB = 0x00FF00FFu;

// round(a * b / 255) для a, b ∈ [0, 255].
inline u32 mul8(u32 a, u32 b) {
  u32 t = a * b + 128;
  return (t + (t >> 8)) >> 8;
}

// Все каналы пикселя p, умноженные на a/255 (с округлением), a ∈ [0, 255].
inline u32 mul(u32 p, u32 a) {
  u32 rb = (p & kRB) * a + 0x00800080u;
  u32 ag = ((p >> 8) & kRB) * a + 0x00800080u;
  rb = ((rb + ((rb >> 8) & kRB)) >> 8) & kRB;
  ag = (ag + ((ag >> 8) & kRB)) & ~kRB;
  return rb | ag;
}

// Линейная интерполяция пикселей, t ∈ [0, 256].
inline u32 lerp(u32 a, u32 b, u32 t) {
  u32 it = 256 - t;
  u32 rb = ((a & kRB) * it + (b & kRB) * t) >> 8;
  u32 ag = ((a >> 8) & kRB) * it + ((b >> 8) & kRB) * t;
  return (rb & kRB) | (ag & ~kRB);
}

// Билинейная выборка из четырёх соседей; fx, fy ∈ [0, 256].
inline u32 bilerp(u32 p00, u32 p10, u32 p01, u32 p11, u32 fx, u32 fy) {
  if (fy == 0) return fx == 0 ? p00 : lerp(p00, p10, fx);
  if (fx == 0) return lerp(p00, p01, fy);
  return lerp(lerp(p00, p10, fx), lerp(p01, p11, fx), fy);
}

inline u32 srcOver(u32 s, u32 d) { return s + mul(d, 255 - (s >> 24)); }

inline u32 div255(u32 x) { return (x + 127) / 255; }

// Multiply (premultiplied): Sc·Dc + Sc·(1−Da) + Dc·(1−Sa).
inline u32 multiply(u32 s, u32 d) {
  u32 sa = s >> 24, da = d >> 24;
  auto ch = [&](int sh) {
    u32 sc = (s >> sh) & 255, dc = (d >> sh) & 255;
    u32 v = div255(sc * dc + sc * (255 - da) + dc * (255 - sa));
    return std::min<u32>(v, 255) << sh;
  };
  u32 a = sa + da - div255(sa * da);
  return (std::min<u32>(a, 255) << 24) | ch(16) | ch(8) | ch(0);
}

// Screen: S + D − S·D (для всех каналов, включая альфу).
inline u32 screen(u32 s, u32 d) {
  auto ch = [&](int sh) {
    u32 sc = (s >> sh) & 255, dc = (d >> sh) & 255;
    return (sc + dc - div255(sc * dc)) << sh;
  };
  return ch(24) | ch(16) | ch(8) | ch(0);
}

// Add: min(1, S + D) по каналам.
inline u32 add(u32 s, u32 d) {
  u32 rb = (s & kRB) + (d & kRB);
  u32 ag = ((s >> 8) & kRB) + ((d >> 8) & kRB);
  // насыщение: переполнение в бит 8 каждого поля -> 255
  rb |= ((rb & 0x01000100u) * 255) >> 8;
  ag |= ((ag & 0x01000100u) * 255) >> 8;
  return (rb & kRB) | ((ag & kRB) << 8);
}

template <Blend B>
inline u32 blend(u32 s, u32 d) {
  if constexpr (B == Blend::Normal) return srcOver(s, d);
  else if constexpr (B == Blend::Multiply) return multiply(s, d);
  else if constexpr (B == Blend::Screen) return screen(s, d);
  else return add(s, d);
}

// ---------------------------------------------------------------- ядра строк
// Normal без ветвлений внутри цикла (компилятор векторизует): s' = s·k, d = s' + d·(1 − a(s')).
// При s' = 0 результат равен d, при непрозрачном s' — s', поэтому особые случаи не нужны.
inline void normalCov(u32* d, int n, u32 s, const u8* cov) {
  for (int i = 0; i < n; i++) {
    const u32 t = mul(s, cov[i]);
    d[i] = t + mul(d[i], 255 - (t >> 24));
  }
}

inline void normalColorsCov(u32* d, int n, const u32* src, const u8* cov) {
  for (int i = 0; i < n; i++) {
    const u32 t = mul(src[i], cov[i]);
    d[i] = t + mul(d[i], 255 - (t >> 24));
  }
}

inline void normalColorsConst(u32* d, int n, const u32* src, u32 c) {
  for (int i = 0; i < n; i++) {
    const u32 t = mul(src[i], c);
    d[i] = t + mul(d[i], 255 - (t >> 24));
  }
}

// Полное покрытие: блоки по 8 непрозрачных пикселей копируются, остальные смешиваются.
inline void normalColors(u32* d, int n, const u32* src) {
  int i = 0;
  for (; i + 8 <= n; i += 8) {
    const u32* s = src + i;
    if ((s[0] & s[1] & s[2] & s[3] & s[4] & s[5] & s[6] & s[7]) >= 0xFF000000u) {
      std::memcpy(d + i, s, 8 * sizeof(u32));
      continue;
    }
    for (int k = 0; k < 8; k++) d[i + k] = s[k] + mul(d[i + k], 255 - (s[k] >> 24));
  }
  for (; i < n; i++) d[i] = src[i] + mul(d[i], 255 - (src[i] >> 24));
}

// Сплошной цвет s (с уже учтённым покрытием) на n пикселях.
template <Blend B>
inline void solidConst(u32* d, int n, u32 s) {
  if constexpr (B == Blend::Normal) {
    if (s >= 0xFF000000u) { std::fill(d, d + n, s); return; }
    if (s == 0) return;
    u32 ia = 255 - (s >> 24);
    for (int i = 0; i < n; i++) d[i] = s + mul(d[i], ia);
  } else {
    if (s == 0) return;
    for (int i = 0; i < n; i++) d[i] = blend<B>(s, d[i]);
  }
}

// Сплошной цвет с попиксельным покрытием.
template <Blend B>
inline void solidCov(u32* d, int n, u32 s, const u8* cov) {
  if constexpr (B == Blend::Normal) {
    normalCov(d, n, s, cov);
  } else {
    for (int i = 0; i < n; i++)
      if (u32 c = cov[i]) d[i] = blend<B>(c == 255 ? s : mul(s, c), d[i]);
  }
}

// Строка цветов src с покрытием cov (nullptr — постоянное c).
template <Blend B>
inline void colors(u32* d, int n, const u32* src, const u8* cov, u32 c) {
  if constexpr (B == Blend::Normal) {
    if (cov) normalColorsCov(d, n, src, cov);
    else if (c == 255) normalColors(d, n, src);
    else if (c) normalColorsConst(d, n, src, c);
  } else if (!cov) {
    if (c == 0) return;
    for (int i = 0; i < n; i++)
      if (u32 s = src[i]) d[i] = blend<B>(c == 255 ? s : mul(s, c), d[i]);
  } else {
    for (int i = 0; i < n; i++) {
      const u32 k = cov[i], s = src[i];
      if (k && s) d[i] = blend<B>(k == 255 ? s : mul(s, k), d[i]);
    }
  }
}

// Диспетчеры по режиму смешивания.
inline void solidConst(Blend b, u32* d, int n, u32 s) {
  switch (b) {
    case Blend::Normal: solidConst<Blend::Normal>(d, n, s); break;
    case Blend::Multiply: solidConst<Blend::Multiply>(d, n, s); break;
    case Blend::Screen: solidConst<Blend::Screen>(d, n, s); break;
    case Blend::Add: solidConst<Blend::Add>(d, n, s); break;
  }
}
inline void solidCov(Blend b, u32* d, int n, u32 s, const u8* cov) {
  switch (b) {
    case Blend::Normal: solidCov<Blend::Normal>(d, n, s, cov); break;
    case Blend::Multiply: solidCov<Blend::Multiply>(d, n, s, cov); break;
    case Blend::Screen: solidCov<Blend::Screen>(d, n, s, cov); break;
    case Blend::Add: solidCov<Blend::Add>(d, n, s, cov); break;
  }
}
inline void colors(Blend b, u32* d, int n, const u32* src, const u8* cov, u32 c) {
  switch (b) {
    case Blend::Normal: colors<Blend::Normal>(d, n, src, cov, c); break;
    case Blend::Multiply: colors<Blend::Multiply>(d, n, src, cov, c); break;
    case Blend::Screen: colors<Blend::Screen>(d, n, src, cov, c); break;
    case Blend::Add: colors<Blend::Add>(d, n, src, cov, c); break;
  }
}

}  // namespace rg::gfx::px
