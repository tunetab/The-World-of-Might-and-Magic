// Regnum — JPEG: декодер baseline/progressive (Хаффман), целочисленное ОДКП (схема Лёффлера), сглаженная
// интерполяция цветности, преобразование цвета, ориентация EXIF.
#include "codec/jpeg.h"

#include "base/fs.h"

namespace rg::codec {

namespace {

// Зигзаг -> естественный порядок; запас в конце защищает от выхода k за 63.
constexpr u8 kNatural[64 + 16] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
    63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63};

inline u16 be16(const u8* p) { return u16((p[0] << 8) | p[1]); }
inline u8 clamp8(i64 v) { return v < 0 ? 0 : v > 255 ? 255 : u8(v); }
inline i16 clamp16(i32 v) { return i16(v < -32768 ? -32768 : v > 32767 ? 32767 : v); }

// ================================================================ Хаффман
struct Huff {
  u16 fast[512];  // (длина << 8) | символ для кодов до 9 бит; 0 — нет
  u32 maxcode[18];
  int delta[17];
  u8 vals[256];
  int count = 0;
  bool defined = false;

  bool build(const u8* counts, const u8* syms, int n) {
    std::memset(fast, 0, sizeof fast);
    std::memcpy(vals, syms, size_t(n));
    count = n;
    u32 code = 0;
    int k = 0;
    for (int l = 1; l <= 16; l++) {
      delta[l] = k - int(code);
      for (int i = 0; i < counts[l - 1]; i++) {
        if (code >= (1u << l)) return false;  // переполненный код
        if (l <= 9) {
          u32 first = code << (9 - l), cnt = 1u << (9 - l);
          for (u32 j = 0; j < cnt; j++) fast[first + j] = u16((l << 8) | vals[k]);
        }
        code++;
        k++;
      }
      maxcode[l] = code << (16 - l);
      code <<= 1;
    }
    maxcode[17] = 0xFFFFFFFFu;
    defined = true;
    return true;
  }
};

// ================================================================ чтение энтропийных данных
struct BitReader {
  const u8* p;
  const u8* end;
  u64 buf = 0;
  int cnt = 0;
  int marker = 0;  // встреченный маркер (p указывает на его 0xFF)
  int zeros = 0;   // байты-нули, поданные после конца данных или маркера

  void fill() {
    while (cnt <= 56) {
      u32 b = 0;
      if (!marker && p < end) {
        b = *p++;
        if (b == 0xFF) {
          while (p < end && *p == 0xFF) p++;
          if (p < end && *p == 0) {
            p++;
          } else {
            marker = p < end ? *p : 0xD9;
            p--;
            b = 0;
            zeros++;
          }
        }
      } else {
        zeros++;
      }
      buf |= u64(b) << (56 - cnt);
      cnt += 8;
    }
  }
  inline void ensure(int n) {
    if (cnt < n) fill();
  }
  inline u32 bits(int n) {  // 1..16
    ensure(n);
    u32 v = u32(buf >> (64 - n));
    buf <<= n;
    cnt -= n;
    return v;
  }
  inline int extend(int s) {  // значение категории s (1..16)
    u32 v = bits(s);
    return v < (1u << (s - 1)) ? int(v) - int((1u << s) - 1) : int(v);
  }
  // Поданы ли «несуществующие» биты сверх запаса (данные кончились).
  bool overrun() const { return zeros * 8 - cnt > 64; }
  void reset() {
    buf = 0;
    cnt = 0;
    zeros = 0;
  }
};

inline int decodeHuff(BitReader& br, const Huff& h) {
  br.ensure(16);
  u32 c16 = u32(br.buf >> 48);
  u16 f = h.fast[c16 >> 7];
  if (f) {
    br.buf <<= (f >> 8);
    br.cnt -= (f >> 8);
    return f & 255;
  }
  for (int l = 10; l <= 16; l++) {
    if (c16 < h.maxcode[l]) {
      int idx = int(c16 >> (16 - l)) + h.delta[l];
      if (idx < 0 || idx >= h.count) return -1;
      br.buf <<= l;
      br.cnt -= l;
      return h.vals[idx];
    }
  }
  return -1;
}

// ================================================================ ОДКП 8×8 (целочисленное, 13 бит констант)
constexpr int kCB = 13, kP1 = 2;
constexpr i64 kF0298 = 2446, kF0390 = 3196, kF0541 = 4433, kF0765 = 6270, kF0899 = 7373, kF1175 = 9633, kF1501 = 12299,
              kF1847 = 15137, kF1961 = 16069, kF2053 = 16819, kF2562 = 20995, kF3072 = 25172;
inline i64 descale(i64 x, int n) { return (x + (i64(1) << (n - 1))) >> n; }

// in — деквантованные коэффициенты в естественном порядке; результат — 8×8 байт со сдвигом +128.
void idct8x8(const i32* in, u8* out, size_t stride) {
  i64 ws[64];
  for (int c = 0; c < 8; c++) {
    const i32* col = in + c;
    if (!(col[8] | col[16] | col[24] | col[32] | col[40] | col[48] | col[56])) {
      i64 dc = i64(col[0]) * (1 << kP1);
      for (int r = 0; r < 8; r++) ws[r * 8 + c] = dc;
      continue;
    }
    i64 z2 = col[16], z3 = col[48];
    i64 z1 = (z2 + z3) * kF0541;
    i64 tmp2 = z1 - z3 * kF1847;
    i64 tmp3 = z1 + z2 * kF0765;
    z2 = col[0];
    z3 = col[32];
    i64 tmp0 = (z2 + z3) * (1 << kCB);
    i64 tmp1 = (z2 - z3) * (1 << kCB);
    i64 t10 = tmp0 + tmp3, t13 = tmp0 - tmp3, t11 = tmp1 + tmp2, t12 = tmp1 - tmp2;
    i64 a0 = col[56], a1 = col[40], a2 = col[24], a3 = col[8];
    i64 o1 = a0 + a3, o2 = a1 + a2, o3 = a0 + a2, o4 = a1 + a3;
    i64 z5 = (o3 + o4) * kF1175;
    a0 *= kF0298;
    a1 *= kF2053;
    a2 *= kF3072;
    a3 *= kF1501;
    o1 *= -kF0899;
    o2 *= -kF2562;
    o3 = o3 * -kF1961 + z5;
    o4 = o4 * -kF0390 + z5;
    a0 += o1 + o3;
    a1 += o2 + o4;
    a2 += o2 + o3;
    a3 += o1 + o4;
    constexpr int s = kCB - kP1;
    ws[0 * 8 + c] = descale(t10 + a3, s);
    ws[7 * 8 + c] = descale(t10 - a3, s);
    ws[1 * 8 + c] = descale(t11 + a2, s);
    ws[6 * 8 + c] = descale(t11 - a2, s);
    ws[2 * 8 + c] = descale(t12 + a1, s);
    ws[5 * 8 + c] = descale(t12 - a1, s);
    ws[3 * 8 + c] = descale(t13 + a0, s);
    ws[4 * 8 + c] = descale(t13 - a0, s);
  }
  for (int r = 0; r < 8; r++) {
    const i64* w = ws + r * 8;
    u8* o = out + size_t(r) * stride;
    if (!(w[1] | w[2] | w[3] | w[4] | w[5] | w[6] | w[7])) {
      u8 v = clamp8(descale(w[0], kP1 + 3) + 128);
      std::memset(o, v, 8);
      continue;
    }
    i64 z2 = w[2], z3 = w[6];
    i64 z1 = (z2 + z3) * kF0541;
    i64 tmp2 = z1 - z3 * kF1847;
    i64 tmp3 = z1 + z2 * kF0765;
    i64 tmp0 = (w[0] + w[4]) * (1 << kCB);
    i64 tmp1 = (w[0] - w[4]) * (1 << kCB);
    i64 t10 = tmp0 + tmp3, t13 = tmp0 - tmp3, t11 = tmp1 + tmp2, t12 = tmp1 - tmp2;
    i64 a0 = w[7], a1 = w[5], a2 = w[3], a3 = w[1];
    i64 o1 = a0 + a3, o2 = a1 + a2, o3 = a0 + a2, o4 = a1 + a3;
    i64 z5 = (o3 + o4) * kF1175;
    a0 *= kF0298;
    a1 *= kF2053;
    a2 *= kF3072;
    a3 *= kF1501;
    o1 *= -kF0899;
    o2 *= -kF2562;
    o3 = o3 * -kF1961 + z5;
    o4 = o4 * -kF0390 + z5;
    a0 += o1 + o3;
    a1 += o2 + o4;
    a2 += o2 + o3;
    a3 += o1 + o4;
    constexpr int s = kCB + kP1 + 3;
    o[0] = clamp8(descale(t10 + a3, s) + 128);
    o[7] = clamp8(descale(t10 - a3, s) + 128);
    o[1] = clamp8(descale(t11 + a2, s) + 128);
    o[6] = clamp8(descale(t11 - a2, s) + 128);
    o[2] = clamp8(descale(t12 + a1, s) + 128);
    o[5] = clamp8(descale(t12 - a1, s) + 128);
    o[3] = clamp8(descale(t13 + a0, s) + 128);
    o[4] = clamp8(descale(t13 - a0, s) + 128);
  }
}

// ================================================================ декодер
struct Component {
  int id = 0, h = 1, v = 1, tq = 0;
  int bw = 0, bh = 0;  // блоков в строке/столбце (с запасом до MCU)
  int cw = 0, ch = 0;  // размер компоненты в отсчётах
  std::vector<u8> plane;   // bw*8 × bh*8
  std::vector<i16> coefs;  // progressive: bw*bh*64 (естественный порядок)
  u16 q[64] = {};          // таблица квантования (зигзаг), закрепляется при первом скане
  bool qLatched = false;
  int pred = 0;
  int dcT = 0, acT = 0;
};

struct Decoder {
  const u8* data = nullptr;
  size_t size = 0;
  std::string err;
  JpegInfo info;
  i64 maxPixels = i64(1) << 28;
  bool infoOnly = false;

  bool frame = false, scanned = false;
  int precision = 8;
  int ncomp = 0, hmax = 1, vmax = 1, mcux = 0, mcuy = 0;
  Component comp[4];
  u16 qt[4][64] = {};
  bool qtDef[4] = {};
  Huff dcT[4], acT[4];
  int restart = 0;
  int adobeTransform = -1;
  int eobrun = 0;

  bool fail(const std::string& m) {
    if (err.empty()) err = "JPEG: " + m;
    return false;
  }

  // ---------------------------------------------------------------- сегменты
  bool parseSOF(const u8* s, size_t n, int type) {
    if (frame) return fail("повторный заголовок кадра");
    if (n < 6) return fail("повреждён заголовок кадра");
    precision = s[0];
    info.h = be16(s + 1);
    info.w = be16(s + 3);
    ncomp = s[5];
    info.components = ncomp;
    info.progressive = type == 0xC2;
    if (precision != 8) return fail(strf("точность %d бит не поддерживается", precision));
    if (info.h == 0) return fail("высота, заданная маркером DNL, не поддерживается");
    if (info.w == 0) return fail("нулевая ширина изображения");
    if (ncomp != 1 && ncomp != 3 && ncomp != 4) return fail(strf("число компонент %d не поддерживается", ncomp));
    if (n < size_t(6 + ncomp * 3)) return fail("повреждён заголовок кадра");
    for (int i = 0; i < ncomp; i++) {
      Component& c = comp[i];
      c.id = s[6 + i * 3];
      c.h = s[7 + i * 3] >> 4;
      c.v = s[7 + i * 3] & 15;
      c.tq = s[8 + i * 3];
      if (c.h < 1 || c.h > 4 || c.v < 1 || c.v > 4) return fail("неверные коэффициенты субдискретизации");
      if (c.tq > 3) return fail("неверный номер таблицы квантования");
      hmax = std::max(hmax, c.h);
      vmax = std::max(vmax, c.v);
    }
    frame = true;
    if (infoOnly) return true;
    if (i64(info.w) * i64(info.h) > maxPixels) return fail(strf("слишком большое изображение (%d × %d)", info.w, info.h));
    mcux = (info.w + 8 * hmax - 1) / (8 * hmax);
    mcuy = (info.h + 8 * vmax - 1) / (8 * vmax);
    for (int i = 0; i < ncomp; i++) {
      Component& c = comp[i];
      if (hmax % c.h || vmax % c.v) return fail("дробная субдискретизация не поддерживается");
      c.bw = mcux * c.h;
      c.bh = mcuy * c.v;
      c.cw = (info.w * c.h + hmax - 1) / hmax;
      c.ch = (info.h * c.v + vmax - 1) / vmax;
      c.plane.assign(size_t(c.bw) * 8 * size_t(c.bh) * 8, 128);
      if (info.progressive) c.coefs.assign(size_t(c.bw) * size_t(c.bh) * 64, 0);
    }
    return true;
  }

  bool parseDQT(const u8* s, size_t n) {
    size_t i = 0;
    while (i < n) {
      int pq = s[i] >> 4, tq = s[i] & 15;
      i++;
      if (tq > 3 || pq > 1) return fail("неверная таблица квантования");
      size_t need = pq ? 128 : 64;
      if (i + need > n) return fail("повреждена таблица квантования");
      for (int k = 0; k < 64; k++) qt[tq][k] = pq ? be16(s + i + size_t(k) * 2) : s[i + size_t(k)];
      qtDef[tq] = true;
      i += need;
    }
    return true;
  }

  bool parseDHT(const u8* s, size_t n) {
    size_t i = 0;
    while (i < n) {
      if (i + 17 > n) return fail("повреждена таблица Хаффмана");
      int tc = s[i] >> 4, th = s[i] & 15;
      if (tc > 1 || th > 3) return fail("неверная таблица Хаффмана");
      const u8* counts = s + i + 1;
      int total = 0;
      for (int k = 0; k < 16; k++) total += counts[k];
      if (total > 256 || i + 17 + size_t(total) > n) return fail("повреждена таблица Хаффмана");
      Huff& h = tc ? acT[th] : dcT[th];
      if (!h.build(counts, s + i + 17, total)) return fail("неверная таблица Хаффмана");
      i += 17 + size_t(total);
    }
    return true;
  }

  void parseExif(const u8* s, size_t n) {
    if (n < 14 || std::memcmp(s, "Exif\0\0", 6) != 0) return;
    const u8* t = s + 6;
    size_t tn = n - 6;
    bool le = t[0] == 'I' && t[1] == 'I';
    if (!le && !(t[0] == 'M' && t[1] == 'M')) return;
    auto r16 = [&](size_t o) -> u32 { return le ? u32(t[o] | (t[o + 1] << 8)) : u32((t[o] << 8) | t[o + 1]); };
    auto r32 = [&](size_t o) -> u32 { return le ? (r16(o) | (r16(o + 2) << 16)) : ((r16(o) << 16) | r16(o + 2)); };
    if (r16(2) != 42) return;
    u32 ifd = r32(4);
    if (u64(ifd) + 2 > tn) return;
    u32 cnt = r16(ifd);
    for (u32 e = 0; e < cnt; e++) {
      size_t o = size_t(ifd) + 2 + size_t(e) * 12;
      if (o + 12 > tn) return;
      if (r16(o) == 0x0112 && r16(o + 2) == 3) {
        u32 v = r16(o + 8);
        if (v >= 1 && v <= 8) info.orientation = int(v);
        return;
      }
    }
  }

  // ---------------------------------------------------------------- энтропийное декодирование блоков
  // Последовательный режим: блок сразу в плоскость компоненты.
  bool blockSeq(BitReader& br, Component& c, int bx, int by) {
    i32 blk[64] = {};
    int t = decodeHuff(br, dcT[c.dcT]);
    if (t < 0 || t > 16) return fail("повреждены данные изображения");
    int diff = t ? (t == 16 ? 32768 : br.extend(t)) : 0;
    c.pred = clamp16(c.pred + diff);
    blk[0] = c.pred * i32(c.q[0]);
    const Huff& ac = acT[c.acT];
    for (int k = 1; k < 64;) {
      int rs = decodeHuff(br, ac);
      if (rs < 0) return fail("повреждены данные изображения");
      int r = rs >> 4, s = rs & 15;
      if (s == 0) {
        if (r != 15) break;
        k += 16;
        continue;
      }
      k += r;
      if (k > 63) return fail("повреждены данные изображения");
      blk[kNatural[k]] = br.extend(s) * i32(c.q[k]);
      k++;
    }
    size_t stride = size_t(c.bw) * 8;
    idct8x8(blk, c.plane.data() + size_t(by) * 8 * stride + size_t(bx) * 8, stride);
    return true;
  }

  bool blockDcFirst(BitReader& br, Component& c, i16* co, int al) {
    int t = decodeHuff(br, dcT[c.dcT]);
    if (t < 0 || t > 16) return fail("повреждены данные изображения");
    int diff = t ? (t == 16 ? 32768 : br.extend(t)) : 0;
    c.pred = clamp16(c.pred + diff);
    co[0] = clamp16(c.pred * (1 << al));
    return true;
  }

  void blockDcRefine(BitReader& br, i16* co, int al) {
    if (br.bits(1)) co[0] = i16(co[0] | (1 << al));
  }

  bool blockAcFirst(BitReader& br, Component& c, i16* co, int ss, int se, int al) {
    if (eobrun > 0) {
      eobrun--;
      return true;
    }
    const Huff& ac = acT[c.acT];
    for (int k = ss; k <= se;) {
      int rs = decodeHuff(br, ac);
      if (rs < 0) return fail("повреждены данные изображения");
      int r = rs >> 4, s = rs & 15;
      if (s == 0) {
        if (r < 15) {
          eobrun = (1 << r) - 1;
          if (r) eobrun += int(br.bits(r));
          break;
        }
        k += 16;
        continue;
      }
      k += r;
      if (k > 63) return fail("повреждены данные изображения");
      co[kNatural[k]] = clamp16(br.extend(s) * (1 << al));
      k++;
    }
    return true;
  }

  bool blockAcRefine(BitReader& br, Component& c, i16* co, int ss, int se, int al) {
    const int p1 = 1 << al, m1 = -1 * (1 << al);
    int k = ss;
    auto refine = [&](i16& v) {
      if (br.bits(1) && (v & p1) == 0) v = clamp16(v >= 0 ? v + p1 : v + m1);
    };
    if (eobrun <= 0) {
      const Huff& ac = acT[c.acT];
      for (; k <= se;) {
        int rs = decodeHuff(br, ac);
        if (rs < 0) return fail("повреждены данные изображения");
        int r = rs >> 4, s = rs & 15;
        int val = 0;
        if (s) {
          if (s != 1) return fail("повреждены данные изображения");
          val = br.bits(1) ? p1 : m1;
        } else if (r != 15) {
          eobrun = 1 << r;
          if (r) eobrun += int(br.bits(r));
          break;
        }
        // пропустить r нулевых коэффициентов, уточняя ненулевые по пути
        while (k <= se) {
          i16& v = co[kNatural[k]];
          if (v != 0) {
            refine(v);
          } else {
            if (r == 0) break;
            r--;
          }
          k++;
        }
        if (val && k <= se) co[kNatural[k]] = i16(val);
        k++;
      }
    }
    if (eobrun > 0) {
      for (; k <= se; k++) {
        i16& v = co[kNatural[k]];
        if (v != 0) refine(v);
      }
      eobrun--;
    }
    return true;
  }

  // ---------------------------------------------------------------- скан
  // Возвращает позицию после энтропийных данных.
  bool scan(const u8* s, size_t n, size_t& pos) {
    if (!frame) return fail("скан до заголовка кадра");
    if (n < 1) return fail("повреждён заголовок скана");
    int ns = s[0];
    if (ns < 1 || ns > ncomp || n < size_t(4 + ns * 2)) return fail("повреждён заголовок скана");
    int idx[4];
    for (int i = 0; i < ns; i++) {
      int cid = s[1 + i * 2], tables = s[2 + i * 2];
      idx[i] = -1;
      for (int j = 0; j < ncomp; j++)
        if (comp[j].id == cid) idx[i] = j;
      if (idx[i] < 0) return fail("скан ссылается на неизвестную компоненту");
      comp[idx[i]].dcT = tables >> 4;
      comp[idx[i]].acT = tables & 15;
      if (comp[idx[i]].dcT > 3 || comp[idx[i]].acT > 3) return fail("неверный номер таблицы Хаффмана");
    }
    int ss = s[1 + ns * 2], se = s[2 + ns * 2], ah = s[3 + ns * 2] >> 4, al = s[3 + ns * 2] & 15;
    const bool prog = info.progressive;
    if (prog) {
      if (ss > se || se > 63 || ah > 13 || al > 13) return fail("неверные параметры прогрессивного скана");
      if (ss == 0 && se != 0) return fail("неверные параметры прогрессивного скана");
      if (ss > 0 && ns != 1) return fail("скан AC должен содержать одну компоненту");
    } else if (ss != 0 || se != 63 || ah != 0 || al != 0) {
      // некоторые кодеры пишут мусор в этих полях последовательного скана — игнорируем
    }
    for (int i = 0; i < ns; i++) {
      Component& c = comp[idx[i]];
      if (!c.qLatched) {
        if (!qtDef[c.tq]) return fail("нет таблицы квантования");
        std::memcpy(c.q, qt[c.tq], sizeof c.q);
        c.qLatched = true;
      }
      bool needDc = !prog || (ss == 0 && ah == 0);
      bool needAc = !prog || ss > 0;
      if (needDc && !dcT[c.dcT].defined) return fail("нет таблицы Хаффмана DC");
      if (needAc && !acT[c.acT].defined) return fail("нет таблицы Хаффмана AC");
      c.pred = 0;
    }
    scanned = true;
    eobrun = 0;
    BitReader br{data + pos, data + size};

    auto doBlock = [&](Component& c, int bx, int by) -> bool {
      if (!prog) return blockSeq(br, c, bx, by);
      i16* co = c.coefs.data() + (size_t(by) * size_t(c.bw) + size_t(bx)) * 64;
      if (ss == 0) {
        if (ah == 0) return blockDcFirst(br, c, co, al);
        blockDcRefine(br, co, al);
        return true;
      }
      if (ah == 0) return blockAcFirst(br, c, co, ss, se, al);
      return blockAcRefine(br, c, co, ss, se, al);
    };
    auto restartMarker = [&]() {
      br.buf = 0;
      br.cnt = 0;
      if (!br.marker) {
        while (br.p + 1 < br.end && !(br.p[0] == 0xFF && br.p[1] != 0 && br.p[1] != 0xFF)) br.p++;
        if (br.p + 1 >= br.end) {
          br.p = br.end;
          br.marker = 0xD9;
          return;
        }
        br.marker = br.p[1];
      }
      if (br.marker >= 0xD0 && br.marker <= 0xD7) {
        br.p += 2;
        br.marker = 0;
        br.zeros = 0;
      }
      for (int i = 0; i < ns; i++) comp[idx[i]].pred = 0;
      eobrun = 0;
    };

    bool ok = true;
    if (ns == 1) {
      Component& c = comp[idx[0]];
      const int bwS = (c.cw + 7) / 8, bhS = (c.ch + 7) / 8;
      long mcu = 0;
      for (int by = 0; by < bhS && ok; by++) {
        for (int bx = 0; bx < bwS; bx++, mcu++) {
          if (restart && mcu > 0 && mcu % restart == 0) restartMarker();
          if (br.overrun()) { ok = false; break; }
          if (!doBlock(c, bx, by)) return false;
        }
      }
    } else {
      long mcu = 0;
      for (int my = 0; my < mcuy && ok; my++) {
        for (int mx = 0; mx < mcux; mx++, mcu++) {
          if (restart && mcu > 0 && mcu % restart == 0) restartMarker();
          if (br.overrun()) { ok = false; break; }
          for (int i = 0; i < ns; i++) {
            Component& c = comp[idx[i]];
            for (int v = 0; v < c.v; v++)
              for (int h = 0; h < c.h; h++)
                if (!doBlock(c, mx * c.h + h, my * c.v + v)) return false;
          }
        }
      }
    }
    // позиция после данных: маркер (если встречен) или далее по потоку
    pos = size_t(br.p - data);
    return true;
  }

  // ---------------------------------------------------------------- разбор файла
  bool run() {
    if (size < 4 || data[0] != 0xFF || data[1] != 0xD8) return fail("неверная подпись файла");
    size_t pos = 2;
    for (;;) {
      // следующий маркер: 0xFF, затем код (не 0x00 и не 0xFF)
      while (pos + 1 < size && !(data[pos] == 0xFF && data[pos + 1] != 0 && data[pos + 1] != 0xFF)) pos++;
      if (pos + 1 >= size) break;
      int code = data[pos + 1];
      pos += 2;
      if (code == 0xD9) break;
      if ((code >= 0xD0 && code <= 0xD8) || code == 0x01) continue;
      if (pos + 2 > size) break;
      size_t len = be16(data + pos);
      if (len < 2 || pos + len > size) {
        if (scanned) break;  // обрезанный хвост
        return fail("повреждён сегмент заголовка");
      }
      const u8* s = data + pos + 2;
      size_t n = len - 2;
      pos += len;
      switch (code) {
        case 0xC0:
        case 0xC1:
        case 0xC2:
          if (!parseSOF(s, n, code)) return false;
          if (infoOnly) return true;
          break;
        case 0xC3: return fail("JPEG без потерь не поддерживается");
        case 0xC5: case 0xC6: case 0xC7: case 0xCD: case 0xCE: case 0xCF:
          return fail("иерархический JPEG не поддерживается");
        case 0xC9: case 0xCA: case 0xCB: case 0xCC:
          return fail("арифметическое кодирование JPEG не поддерживается");
        case 0xC4:
          if (!parseDHT(s, n)) return false;
          break;
        case 0xDB:
          if (!parseDQT(s, n)) return false;
          break;
        case 0xDD:
          if (n < 2) return fail("повреждён интервал рестарта");
          restart = be16(s);
          break;
        case 0xDA:
          if (infoOnly) return true;
          if (!scan(s, n, pos)) return false;
          break;
        case 0xE1:
          parseExif(s, n);
          break;
        case 0xEE:
          if (n >= 12 && std::memcmp(s, "Adobe", 5) == 0) adobeTransform = s[11];
          break;
        default:
          break;  // APPn, COM, DNL и прочие — пропуск
      }
    }
    if (!frame) return fail("нет заголовка кадра");
    if (!infoOnly && !scanned) return fail("нет данных изображения");
    return true;
  }

  // Прогрессивный режим: деквантование и ОДКП всех блоков после сканов.
  void finishProgressive() {
    for (int i = 0; i < ncomp; i++) {
      Component& c = comp[i];
      if (!c.qLatched) {
        if (qtDef[c.tq]) std::memcpy(c.q, qt[c.tq], sizeof c.q);
        else continue;
      }
      u16 qn[64];
      for (int k = 0; k < 64; k++) qn[kNatural[k]] = c.q[k];
      size_t stride = size_t(c.bw) * 8;
      for (int by = 0; by < c.bh; by++)
        for (int bx = 0; bx < c.bw; bx++) {
          const i16* co = c.coefs.data() + (size_t(by) * size_t(c.bw) + size_t(bx)) * 64;
          i32 blk[64];
          for (int k = 0; k < 64; k++) blk[k] = i32(co[k]) * i32(qn[k]);
          idct8x8(blk, c.plane.data() + size_t(by) * 8 * stride + size_t(bx) * 8, stride);
        }
      c.coefs = std::vector<i16>();
    }
  }
};

// ================================================================ интерполяция цветности и цвет
// Билинейная интерполяция с центрированными отсчётами (для коэффициента 2 — «треугольный» фильтр libjpeg).
struct Upsampler {
  const Component* c = nullptr;
  int fx = 1, fy = 1, W = 0;
  std::vector<u32> col;
  std::vector<u8> out;
  std::vector<int> x0, x1;
  std::vector<u16> w0, w1;

  static int floorDiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

  void init(const Component& comp, int hmax, int vmax, int width) {
    c = &comp;
    fx = hmax / comp.h;
    fy = vmax / comp.v;
    W = width;
    if (fx == 1 && fy == 1) return;
    col.resize(size_t(comp.cw));
    out.resize(size_t(W));
    x0.resize(size_t(W));
    x1.resize(size_t(W));
    w0.resize(size_t(W));
    w1.resize(size_t(W));
    for (int x = 0; x < W; x++) {
      int num = 2 * x + 1 - fx;
      int j = floorDiv(num, 2 * fx);
      int rem = num - j * 2 * fx;
      x0[size_t(x)] = std::clamp(j, 0, comp.cw - 1);
      x1[size_t(x)] = std::clamp(j + 1, 0, comp.cw - 1);
      w0[size_t(x)] = u16(2 * fx - rem);
      w1[size_t(x)] = u16(rem);
    }
  }

  const u8* row(int y) {
    const size_t stride = size_t(c->bw) * 8;
    if (fx == 1 && fy == 1) return c->plane.data() + size_t(y) * stride;
    int num = 2 * y + 1 - fy;
    int i = floorDiv(num, 2 * fy);
    int rem = num - i * 2 * fy;
    const u8* r0 = c->plane.data() + size_t(std::clamp(i, 0, c->ch - 1)) * stride;
    const u8* r1 = c->plane.data() + size_t(std::clamp(i + 1, 0, c->ch - 1)) * stride;
    u32 a = u32(2 * fy - rem), b = u32(rem);
    for (int x = 0; x < c->cw; x++) col[size_t(x)] = a * r0[x] + b * r1[x];
    const u32 D = u32(4 * fx * fy), half = D / 2;
    if (fx == 1) {
      for (int x = 0; x < W; x++) out[size_t(x)] = u8((col[size_t(x)] * 2 + half) / D);
    } else {
      for (int x = 0; x < W; x++)
        out[size_t(x)] = u8((w0[size_t(x)] * col[size_t(x0[size_t(x)])] + w1[size_t(x)] * col[size_t(x1[size_t(x)])] + half) / D);
    }
    return out.data();
  }
};

struct YccTables {
  int crR[256], cbB[256], crG[256], cbG[256];
  YccTables() {
    constexpr int one = 1 << 16, half = 1 << 15;
    for (int i = 0; i < 256; i++) {
      int x = i - 128;
      crR[i] = (int(1.40200 * one + 0.5) * x + half) >> 16;
      cbB[i] = (int(1.77200 * one + 0.5) * x + half) >> 16;
      crG[i] = -int(0.71414 * one + 0.5) * x;
      cbG[i] = -int(0.34414 * one + 0.5) * x + half;
    }
  }
};
const YccTables& ycc() {
  static const YccTables t;
  return t;
}

inline u8 mul255(int a, int b) {
  int t = a * b + 128;
  return u8((t + (t >> 8)) >> 8);
}

RgbaImage convert(Decoder& d) {
  RgbaImage img;
  const int W = d.info.w, H = d.info.h;
  img.w = W;
  img.h = H;
  img.rgba.resize(size_t(W) * size_t(H) * 4);
  Upsampler up[4];
  for (int i = 0; i < d.ncomp; i++) up[i].init(d.comp[i], d.hmax, d.vmax, W);
  enum { Gray, Ycc, Rgb, Cmyk, Ycck } mode;
  if (d.ncomp == 1) mode = Gray;
  else if (d.ncomp == 3) {
    if (d.adobeTransform == 0) mode = Rgb;
    else if (d.adobeTransform < 0 && d.comp[0].id == 'R' && d.comp[1].id == 'G' && d.comp[2].id == 'B') mode = Rgb;
    else mode = Ycc;
  } else {
    mode = d.adobeTransform == 2 ? Ycck : Cmyk;
  }
  const bool inverted = d.adobeTransform >= 0;  // Adobe хранит CMYK инвертированным
  const YccTables& T = ycc();
  for (int y = 0; y < H; y++) {
    const u8* r[4] = {};
    for (int i = 0; i < d.ncomp; i++) r[i] = up[i].row(y);
    u8* o = img.rgba.data() + size_t(y) * size_t(W) * 4;
    switch (mode) {
      case Gray:
        for (int x = 0; x < W; x++, o += 4) { o[0] = o[1] = o[2] = r[0][x]; o[3] = 255; }
        break;
      case Rgb:
        for (int x = 0; x < W; x++, o += 4) { o[0] = r[0][x]; o[1] = r[1][x]; o[2] = r[2][x]; o[3] = 255; }
        break;
      case Ycc:
        for (int x = 0; x < W; x++, o += 4) {
          int Y = r[0][x], cb = r[1][x], cr = r[2][x];
          o[0] = clamp8(Y + T.crR[cr]);
          o[1] = clamp8(Y + ((T.cbG[cb] + T.crG[cr]) >> 16));
          o[2] = clamp8(Y + T.cbB[cb]);
          o[3] = 255;
        }
        break;
      case Cmyk:
      case Ycck:
        for (int x = 0; x < W; x++, o += 4) {
          int c0 = r[0][x], c1 = r[1][x], c2 = r[2][x], k = r[3][x];
          if (mode == Ycck) {
            int Y = c0, cb = c1, cr = c2;
            c0 = clamp8(Y + T.crR[cr]);
            c1 = clamp8(Y + ((T.cbG[cb] + T.crG[cr]) >> 16));
            c2 = clamp8(Y + T.cbB[cb]);
          }
          if (!inverted) {
            c0 = 255 - c0;
            c1 = 255 - c1;
            c2 = 255 - c2;
            k = 255 - k;
          }
          o[0] = mul255(c0, k);
          o[1] = mul255(c1, k);
          o[2] = mul255(c2, k);
          o[3] = 255;
        }
        break;
    }
  }
  return img;
}

// Поворот/отражение по тегу EXIF Orientation.
RgbaImage orient(RgbaImage src, int o) {
  if (o <= 1 || o > 8) return src;
  const int w = src.w, h = src.h;
  const bool swap = o >= 5;
  RgbaImage dst;
  dst.w = swap ? h : w;
  dst.h = swap ? w : h;
  dst.rgba.resize(src.rgba.size());
  const u8* s = src.rgba.data();
  for (int y = 0; y < dst.h; y++) {
    u8* row = dst.rgba.data() + size_t(y) * size_t(dst.w) * 4;
    for (int x = 0; x < dst.w; x++) {
      int sx, sy;
      switch (o) {
        case 2: sx = w - 1 - x; sy = y; break;
        case 3: sx = w - 1 - x; sy = h - 1 - y; break;
        case 4: sx = x; sy = h - 1 - y; break;
        case 5: sx = y; sy = x; break;
        case 6: sx = y; sy = h - 1 - x; break;
        case 7: sx = w - 1 - y; sy = h - 1 - x; break;
        default: sx = w - 1 - y; sy = x; break;  // 8
      }
      std::memcpy(row + size_t(x) * 4, s + (size_t(sy) * size_t(w) + size_t(sx)) * 4, 4);
    }
  }
  return dst;
}

}  // namespace

std::optional<JpegInfo> jpegInfo(const u8* data, size_t size, std::string* error) {
  auto d = std::make_unique<Decoder>();
  d->data = data;
  d->size = size;
  d->infoOnly = true;
  if (!data || !d->run()) {
    if (error) *error = d->err.empty() ? "JPEG: неверный файл" : d->err;
    return std::nullopt;
  }
  return d->info;
}

std::optional<RgbaImage> decodeJpeg(const u8* data, size_t size, const JpegDecodeOptions& opt, std::string* error) {
  auto d = std::make_unique<Decoder>();
  d->data = data;
  d->size = size;
  d->maxPixels = opt.maxPixels;
  if (!data || !d->run()) {
    if (error) *error = d->err.empty() ? "JPEG: неверный файл" : d->err;
    return std::nullopt;
  }
  if (d->info.progressive) d->finishProgressive();
  RgbaImage img = convert(*d);
  int o = d->info.orientation;
  d.reset();
  if (opt.applyOrientation) img = orient(std::move(img), o);
  return img;
}

std::optional<RgbaImage> decodeImage(const u8* data, size_t size, std::string* error) {
  if (data && size >= 8 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G') return decodePng(data, size, error);
  if (data && size >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) return decodeJpeg(data, size, JpegDecodeOptions{}, error);
  if (error) *error = "Неизвестный формат изображения (ожидается PNG или JPEG)";
  return std::nullopt;
}

std::optional<RgbaImage> readImageFile(const std::string& path, std::string* error) {
  auto bytes = fs::readFile(path, error);
  if (!bytes) return std::nullopt;
  return decodeImage(*bytes, error);
}

}  // namespace rg::codec
